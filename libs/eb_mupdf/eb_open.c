#include "eb_internal.h"

#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#define EB_MAX_PATH_CHARS 32767
#define EB_MAX_MAGIC_CHARS 15

static int is_sep(wchar_t c) { return c == L'\\' || c == L'/'; }

static int is_device_prefix(const wchar_t* p)
{
    if (!is_sep(p[0]) || !is_sep(p[1])) return 0;
    if (p[2] == L'.' && is_sep(p[3])) return 1;
    return p[2] == L'?' && is_sep(p[3]) && _wcsnicmp(p + 4, L"GLOBALROOT", 10) == 0;
}

static int is_reserved_name(const wchar_t* path)
{
    static const wchar_t* const names[] = {L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"};
    const wchar_t* base = path;
    wchar_t name[16];
    size_t n = 0;
    for (const wchar_t* p = path; *p; ++p) {
        if (is_sep(*p)) base = p + 1;
    }
    if (base[0] && base[1] == L':' && base == path) base += 2;
    while (base[n] && base[n] != L'.' && base[n] != L':' && n < 15) {
        name[n] = base[n];
        ++n;
    }
    while (n > 0 && name[n - 1] == L' ') --n;
    name[n] = L'\0';
    for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
        if (_wcsicmp(name, names[i]) == 0) return 1;
    }
    return n == 4 && (_wcsnicmp(name, L"COM", 3) == 0 || _wcsnicmp(name, L"LPT", 3) == 0) && name[3] >= L'1' && name[3] <= L'9';
}

static eb_status open_file(const wchar_t* path, FILE** out)
{
    LARGE_INTEGER size;
    int fd = -1;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return eb_fail_msg(EB_E_OPEN, "cannot open file");
    if (GetFileType(h) != FILE_TYPE_DISK || !GetFileSizeEx(h, &size)) {
        CloseHandle(h);
        return eb_fail_msg(EB_E_OPEN, "not a regular file");
    }
    if (size.QuadPart < 0 || (uint64_t)size.QuadPart > eb_g_limits.max_file_bytes) {
        CloseHandle(h);
        return eb_fail_msg(EB_E_LIMIT, "file too large");
    }
    fd = _open_osfhandle((intptr_t)h, _O_RDONLY | _O_BINARY);
    if (fd < 0) {
        CloseHandle(h);
        return eb_fail_msg(EB_E_OPEN, "cannot wrap file handle");
    }
    *out = _fdopen(fd, "rb");
    if (!*out) {
        _close(fd);
        return eb_fail_msg(EB_E_OPEN, "cannot open file stream");
    }
    return EB_OK;
}

static void extension_magic(const wchar_t* path, char out[EB_MAX_MAGIC_CHARS + 1])
{
    const wchar_t* dot = NULL;
    size_t n = 0;
    out[0] = '\0';
    for (const wchar_t* p = path; *p; ++p) {
        if (is_sep(*p)) dot = NULL;
        else if (*p == L'.') dot = p;
    }
    if (!dot) return;
    for (; dot[n] && n < EB_MAX_MAGIC_CHARS; ++n) {
        if (dot[n] > 127) {
            out[0] = '\0';
            return;
        }
        out[n] = (char)dot[n];
    }
    out[dot[n] ? 0 : n] = '\0';
}

void eb_close(eb_doc* doc)
{
    if (!doc) return;
    fz_drop_document(doc->ctx, doc->doc);
    if (doc->file) fclose(doc->file);
    eb_release_ctx(doc->ctx);
    free(doc);
}

void eb_abort(eb_doc* doc)
{
    if (doc) InterlockedExchange((volatile LONG*)&doc->cookie.abort, 1);
}

void eb_begin_call(eb_doc* doc)
{
    InterlockedExchange((volatile LONG*)&doc->cookie.abort, 0);
    doc->cookie.progress = 0;
    doc->cookie.errors = 0;
    doc->cookie.incomplete = 0;
}

void eb_check_abort(eb_doc* doc)
{
    if (InterlockedCompareExchange((volatile LONG*)&doc->cookie.abort, 0, 0)) fz_throw(doc->ctx, FZ_ERROR_ABORT, "cancelled");
}

/* Takes ownership of ctx, fdoc and file on every path. */
static eb_status adopt(fz_context* ctx, fz_document* fdoc, FILE* file, eb_doc** out)
{
    eb_status st = EB_OK;
    int drm = 0;
    eb_doc* d = calloc(1, sizeof *d);
    if (!d) {
        fz_drop_document(ctx, fdoc);
        if (file) fclose(file);
        eb_release_ctx(ctx);
        return eb_fail_msg(EB_E_NOMEM, "out of memory");
    }
    d->ctx = ctx;
    d->doc = fdoc;
    d->file = file;
    fz_try(ctx)
    {
        drm = fz_needs_password(ctx, fdoc);
        if (!drm) d->reflowable = fz_is_document_reflowable(ctx, fdoc);
    }
    fz_catch(ctx) st = eb_fail(ctx);
    if (st == EB_OK && drm) st = eb_fail_msg(EB_E_DRM, "document is encrypted");
    if (st == EB_OK) *out = d;
    else eb_close(d);
    return st;
}

static eb_status open_stream(fz_context* ctx, FILE* file, const char* magic, fz_document** fdoc)
{
    fz_stream* stm = NULL;
    eb_status st = EB_OK;
    fz_var(stm);
    fz_try(ctx)
    {
        stm = fz_open_file_ptr_no_close(ctx, file);
        *fdoc = fz_open_document_with_stream(ctx, magic, stm);
    }
    fz_always(ctx) fz_drop_stream(ctx, stm);
    fz_catch(ctx) st = eb_fail(ctx);
    return st;
}

eb_status eb_open(const wchar_t* path, eb_doc** out)
{
    fz_document* fdoc = NULL;
    fz_context* ctx = NULL;
    FILE* file = NULL;
    char magic[EB_MAX_MAGIC_CHARS + 1];
    eb_status st = EB_OK;
    if (!out) return eb_fail_msg(EB_E_ARG, "null out");
    *out = NULL;
    if (!path || wcsnlen(path, EB_MAX_PATH_CHARS + 1) > EB_MAX_PATH_CHARS) return eb_fail_msg(EB_E_ARG, "bad path");
    if (is_device_prefix(path) || is_reserved_name(path)) return eb_fail_msg(EB_E_ARG, "device paths are not allowed");
    ctx = eb_acquire_ctx();
    if (!ctx) return eb_take_oom() ? eb_fail_msg(EB_E_NOMEM, "out of memory") : eb_fail_msg(EB_E_ARG, "eb_init not called");
    st = open_file(path, &file);
    if (st == EB_OK) {
        extension_magic(path, magic);
        st = open_stream(ctx, file, magic, &fdoc);
    }
    if (st == EB_OK) return adopt(ctx, fdoc, file, out);
    if (file) fclose(file);
    eb_release_ctx(ctx);
    return st;
}

eb_status eb_open_memory(const void* data, size_t size, const char* magic, eb_doc** out)
{
    fz_document* fdoc = NULL;
    fz_buffer* buf = NULL;
    fz_stream* stm = NULL;
    fz_context* ctx = NULL;
    eb_status st = EB_OK;
    if (!out) return eb_fail_msg(EB_E_ARG, "null out");
    *out = NULL;
    if (!data || !size || !magic) return eb_fail_msg(EB_E_ARG, "bad memory arguments");
    if (size > eb_g_limits.max_file_bytes) return eb_fail_msg(EB_E_LIMIT, "data too large");
    ctx = eb_acquire_ctx();
    if (!ctx) return eb_take_oom() ? eb_fail_msg(EB_E_NOMEM, "out of memory") : eb_fail_msg(EB_E_ARG, "eb_init not called");
    fz_var(buf);
    fz_var(stm);
    fz_try(ctx)
    {
        buf = fz_new_buffer_from_copied_data(ctx, data, size);
        stm = fz_open_buffer(ctx, buf);
        fdoc = fz_open_document_with_stream(ctx, magic, stm);
    }
    fz_always(ctx)
    {
        fz_drop_stream(ctx, stm);
        fz_drop_buffer(ctx, buf);
    }
    fz_catch(ctx) st = eb_fail(ctx);
    if (st == EB_OK) return adopt(ctx, fdoc, NULL, out);
    eb_release_ctx(ctx);
    return st;
}
