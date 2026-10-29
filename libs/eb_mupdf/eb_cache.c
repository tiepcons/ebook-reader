#include "eb_internal.h"

#include <io.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#define EB_CACHE_MAGIC 0x434C4245u /* "EBLC" */
#define EB_CACHE_VERSION 2u
#define EB_CACHE_HEADER 48u
#define EB_CACHE_MAX_BYTES ((size_t)1 << 20)
#define EB_CACHE_MAX_PATH_CHARS 32700
/* Bump when a change in this DLL (not in MuPDF or the fonts) can alter how a chapter paginates. */
#define EB_LAYOUT_ENGINE_VERSION 1u

#define ACCEL_MAGIC 0xacce1e7au
#define ACCEL_MAGIC_EPUB 0x62755065u
#define ACCEL_VERSION 0x00010001u
#define ACCEL_FIXED_BYTES 36u
#define ACCEL_MAX_CHAPTERS 100000u

static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

static void wr32(uint8_t* p, uint32_t v)
{
    for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));
}

static void wr64(uint8_t* p, uint64_t v)
{
    wr32(p, (uint32_t)v);
    wr32(p + 4, (uint32_t)(v >> 32));
}

void eb_stamp_of(FILE* f, eb_stamp* st)
{
    BY_HANDLE_FILE_INFORMATION bi;
    const intptr_t h = _get_osfhandle(_fileno(f));
    memset(st, 0, sizeof *st);
    if (h == -1 || !GetFileInformationByHandle((HANDLE)h, &bi)) return;
    st->size = (uint64_t)bi.nFileSizeHigh << 32 | bi.nFileSizeLow;
    st->mtime = (uint64_t)bi.ftLastWriteTime.dwHighDateTime << 32 | bi.ftLastWriteTime.dwLowDateTime;
    st->volume = bi.dwVolumeSerialNumber;
    st->index = (uint64_t)bi.nFileIndexHigh << 32 | bi.nFileIndexLow;
    st->valid = 1;
}

/* Page counts depend on the engine, the fonts that were found and the DLL's own layout code. */
static uint64_t env_digest(void)
{
    const uint64_t fonts = eb_font_digest();
    const uint32_t engine = EB_LAYOUT_ENGINE_VERSION;
    uint64_t h = 14695981039346656037ull;
    for (const char* p = FZ_VERSION; *p; ++p) h = (h ^ (unsigned char)*p) * 1099511628211ull;
    for (int i = 0; i < 4; ++i) h = (h ^ (uint8_t)(engine >> (8 * i))) * 1099511628211ull;
    for (int i = 0; i < 8; ++i) h = (h ^ (uint8_t)(fonts >> (8 * i))) * 1099511628211ull;
    return h;
}

static void make_header(uint8_t head[EB_CACHE_HEADER], const eb_stamp* stamp)
{
    wr32(head, EB_CACHE_MAGIC);
    wr32(head + 4, EB_CACHE_VERSION);
    wr64(head + 8, stamp->size);
    wr64(head + 16, stamp->mtime);
    wr64(head + 24, stamp->volume);
    wr64(head + 32, stamp->index);
    wr64(head + 40, env_digest());
}

/* MuPDF trusts the blob it is given, so the layout of an EPUB accelerator is checked here. */
static int accel_ok(const uint8_t* b, size_t n)
{
    uint32_t chapters = 0;
    uint64_t sum = 0;
    if (n < ACCEL_FIXED_BYTES) return 0;
    if (rd32(b) != ACCEL_MAGIC || rd32(b + 4) != ACCEL_MAGIC_EPUB || rd32(b + 8) != ACCEL_VERSION) return 0;
    chapters = rd32(b + 32);
    if (chapters < 1 || chapters > ACCEL_MAX_CHAPTERS || n != ACCEL_FIXED_BYTES + 4u * (size_t)chapters) return 0;
    for (uint32_t i = 0; i < chapters; ++i) {
        const int32_t c = (int32_t)rd32(b + ACCEL_FIXED_BYTES + 4u * i);
        if (c == -1) continue;
        if (c < 0 || (uint32_t)c > eb_g_limits.max_pages) return 0;
        sum += (uint32_t)c;
    }
    return sum <= eb_g_limits.max_pages;
}

static uint8_t* read_small(const wchar_t* path, size_t* size)
{
    LARGE_INTEGER len;
    BY_HANDLE_FILE_INFORMATION bi;
    DWORD got = 0;
    uint8_t* data = NULL;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (GetFileType(h) == FILE_TYPE_DISK && GetFileInformationByHandle(h, &bi) &&
        !(bi.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) && GetFileSizeEx(h, &len) &&
        len.QuadPart > (LONGLONG)EB_CACHE_HEADER && len.QuadPart <= (LONGLONG)EB_CACHE_MAX_BYTES) {
        data = malloc((size_t)len.QuadPart);
        if (data && (!ReadFile(h, data, (DWORD)len.QuadPart, &got, NULL) || got != (DWORD)len.QuadPart)) {
            free(data);
            data = NULL;
        }
        if (data) *size = (size_t)len.QuadPart;
    }
    CloseHandle(h);
    return data;
}

uint8_t* eb_cache_load(const wchar_t* cache_path, const eb_stamp* stamp, size_t* size)
{
    size_t n = 0;
    uint8_t* data = NULL;
    if (!cache_path || !stamp->valid || wcsnlen(cache_path, EB_CACHE_MAX_PATH_CHARS + 1) > EB_CACHE_MAX_PATH_CHARS) return NULL;
    data = read_small(cache_path, &n);
    if (!data) return NULL;
    uint8_t want[EB_CACHE_HEADER];
    make_header(want, stamp);
    if (memcmp(data, want, EB_CACHE_HEADER) != 0 || !accel_ok(data + EB_CACHE_HEADER, n - EB_CACHE_HEADER)) {
        free(data);
        return NULL;
    }
    memmove(data, data + EB_CACHE_HEADER, n - EB_CACHE_HEADER);
    *size = n - EB_CACHE_HEADER;
    return data;
}

static eb_status write_file(const wchar_t* tmp, const uint8_t* head, const uint8_t* body, size_t body_size)
{
    DWORD put = 0;
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    int ok = 0;
    if (h == INVALID_HANDLE_VALUE) return eb_fail_msg(EB_E_OPEN, "cannot create cache file");
    ok = WriteFile(h, head, EB_CACHE_HEADER, &put, NULL) && put == EB_CACHE_HEADER &&
         WriteFile(h, body, (DWORD)body_size, &put, NULL) && put == (DWORD)body_size;
    if (!CloseHandle(h)) ok = 0;
    if (!ok) DeleteFileW(tmp);
    return ok ? EB_OK : eb_fail_msg(EB_E_OPEN, "cannot write cache file");
}

static eb_status publish(const wchar_t* cache_path, const eb_stamp* stamp, const uint8_t* body, size_t body_size)
{
    static volatile LONG counter;
    wchar_t tmp[EB_CACHE_MAX_PATH_CHARS + 64];
    uint8_t head[EB_CACHE_HEADER];
    eb_status st = EB_OK;
    make_header(head, stamp);
    if (swprintf_s(tmp, sizeof tmp / sizeof tmp[0], L"%ls.%lu.%ld.tmp", cache_path, GetCurrentProcessId(),
                   InterlockedIncrement(&counter)) < 0)
        return eb_fail_msg(EB_E_ARG, "bad cache path");
    st = write_file(tmp, head, body, body_size);
    if (st != EB_OK) return st;
    if (!MoveFileExW(tmp, cache_path, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp);
        return eb_fail_msg(EB_E_OPEN, "cannot replace cache file");
    }
    return EB_OK;
}

static eb_status make_blob(eb_doc* doc, fz_buffer** buf)
{
    eb_status st = EB_OK;
    fz_try(doc->ctx)
    {
        if (!fz_document_supports_accelerator(doc->ctx, doc->doc)) {
            st = eb_fail_msg(EB_E_FORMAT, "document has no layout cache");
        } else {
            *buf = fz_new_buffer(doc->ctx, 4096);
            fz_output_accelerator(doc->ctx, doc->doc, fz_new_output_with_buffer(doc->ctx, *buf));
        }
    }
    fz_catch(doc->ctx) st = eb_fail(doc->ctx);
    return st;
}

eb_status eb_save_cache(eb_doc* doc, const wchar_t* cache_path)
{
    fz_buffer* buf = NULL;
    const uint8_t* data = NULL;
    size_t size = 0;
    eb_status st = EB_OK;
    if (!doc || !cache_path || wcsnlen(cache_path, EB_CACHE_MAX_PATH_CHARS + 1) > EB_CACHE_MAX_PATH_CHARS) return eb_fail_msg(EB_E_ARG, "bad argument");
    if (!doc->stamp.valid || !doc->reflowable) return eb_fail_msg(EB_E_FORMAT, "document has no layout cache");
    st = require_layout(doc);
    if (st != EB_OK) return st;
    st = make_blob(doc, &buf);
    if (st == EB_OK) {
        fz_try(doc->ctx) size = fz_buffer_storage(doc->ctx, buf, (unsigned char**)&data);
        fz_catch(doc->ctx) st = eb_fail(doc->ctx);
    }
    if (st == EB_OK && !accel_ok(data, size)) st = eb_fail_msg(EB_E_LIMIT, "layout cache not storable");
    if (st == EB_OK) st = publish(cache_path, &doc->stamp, data, size);
    fz_drop_buffer(doc->ctx, buf);
    return st;
}
