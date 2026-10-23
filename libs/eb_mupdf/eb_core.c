#include "eb_internal.h"

#include <math.h>
#include <string.h>
#include <windows.h>

#define EB_MAX_PATH_CHARS 32767
#define EB_MAX_CSS_BYTES (1u << 20)
#define EB_MAX_LAYOUT_PT 100000.0f
#define EB_MAX_META_BYTES 4096

eb_limits eb_g_limits;
static fz_context* g_base;
static CRITICAL_SECTION g_cs[FZ_LOCK_MAX];
static fz_locks_context g_locks;
static __declspec(thread) char t_err[512];

static void lock_cb(void* user, int i) { EnterCriticalSection(&((CRITICAL_SECTION*)user)[i]); }
static void unlock_cb(void* user, int i) { LeaveCriticalSection(&((CRITICAL_SECTION*)user)[i]); }
static void silent_cb(void* user, const char* message)
{
    (void)user;
    (void)message;
}

eb_status eb_fail_msg(eb_status s, const char* msg)
{
    strncpy_s(t_err, sizeof t_err, msg ? msg : "", _TRUNCATE);
    return s;
}

eb_status eb_fail(fz_context* ctx)
{
    const char* msg = fz_caught_message(ctx);
    switch (fz_caught(ctx)) {
    case FZ_ERROR_LIMIT: return eb_fail_msg(EB_E_LIMIT, msg);
    case FZ_ERROR_ARGUMENT: return eb_fail_msg(EB_E_ARG, msg);
    case FZ_ERROR_LIBRARY:
    case FZ_ERROR_UNSUPPORTED:
    case FZ_ERROR_FORMAT:
    case FZ_ERROR_SYNTAX: return eb_fail_msg(EB_E_FORMAT, msg);
    case FZ_ERROR_SYSTEM: return eb_fail_msg(msg && strstr(msg, "memory") ? EB_E_NOMEM : EB_E_OPEN, msg);
    default: return eb_fail_msg(EB_E_INTERNAL, msg);
    }
}

int32_t eb_abi_version(void) { return EB_ABI_VERSION; }
const char* eb_engine_version(void) { return FZ_VERSION; }

const char* eb_last_error(void) { return t_err; }

const char* eb_status_text(eb_status s)
{
    static const char* const text[] = {"ok", "invalid argument", "cannot read file", "unsupported or corrupt document",
                                       "encrypted document", "limit exceeded", "out of memory", "out of range",
                                       "internal error"};
    return (unsigned)s < sizeof text / sizeof text[0] ? text[s] : "unknown status";
}

eb_status eb_init(const eb_limits* limits)
{
    int i = 0;
    eb_status st = EB_OK;
    if (g_base) return eb_fail_msg(EB_E_ARG, "already initialised");
    eb_g_limits = (eb_limits){2ull << 30, 100000, 64u << 20, (size_t)128 << 20};
    if (limits) {
        if (limits->max_file_bytes) eb_g_limits.max_file_bytes = limits->max_file_bytes;
        if (limits->max_pages) eb_g_limits.max_pages = limits->max_pages;
        if (limits->max_bitmap_px) eb_g_limits.max_bitmap_px = limits->max_bitmap_px;
        if (limits->store_bytes) eb_g_limits.store_bytes = limits->store_bytes;
    }
    for (i = 0; i < FZ_LOCK_MAX; ++i) InitializeCriticalSection(&g_cs[i]);
    g_locks = (fz_locks_context){g_cs, lock_cb, unlock_cb};
    g_base = fz_new_context(NULL, &g_locks, eb_g_limits.store_bytes);
    if (!g_base) st = eb_fail_msg(EB_E_NOMEM, "cannot create context");
    if (g_base) {
        fz_set_warning_callback(g_base, silent_cb, NULL);
        fz_set_error_callback(g_base, silent_cb, NULL);
        fz_try(g_base) fz_register_document_handlers(g_base);
        fz_catch(g_base) st = eb_fail(g_base);
    }
    if (st != EB_OK) {
        fz_drop_context(g_base);
        g_base = NULL;
        for (i = 0; i < FZ_LOCK_MAX; ++i) DeleteCriticalSection(&g_cs[i]);
    }
    return st;
}

void eb_shutdown(void)
{
    if (!g_base) return;
    fz_drop_context(g_base);
    g_base = NULL;
    for (int i = 0; i < FZ_LOCK_MAX; ++i) DeleteCriticalSection(&g_cs[i]);
}

static eb_status adopt(fz_context* ctx, fz_document* fdoc, eb_doc** out)
{
    eb_status st = EB_OK;
    int drm = 0;
    int n = 0;
    eb_doc* d = calloc(1, sizeof *d);
    if (!d) {
        fz_drop_document(ctx, fdoc);
        fz_drop_context(ctx);
        return eb_fail_msg(EB_E_NOMEM, "out of memory");
    }
    d->ctx = ctx;
    d->doc = fdoc;
    fz_try(ctx)
    {
        drm = fz_needs_password(ctx, fdoc);
        if (!drm) {
            d->reflowable = fz_is_document_reflowable(ctx, fdoc);
            if (d->reflowable) fz_layout_document(ctx, fdoc, 400, 600, 12);
            n = fz_count_pages(ctx, fdoc);
        }
    }
    fz_catch(ctx) st = eb_fail(ctx);
    if (st == EB_OK && drm) st = eb_fail_msg(EB_E_DRM, "document is encrypted");
    if (st == EB_OK && (n < 0 || (uint32_t)n > eb_g_limits.max_pages)) st = eb_fail_msg(EB_E_LIMIT, "too many pages");
    if (st == EB_OK) *out = d;
    else eb_close(d);
    return st;
}

static eb_status check_file(const wchar_t* path)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    uint64_t size = 0;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) return eb_fail_msg(EB_E_OPEN, "file not found");
    if (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return eb_fail_msg(EB_E_OPEN, "path is a directory");
    size = ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    if (size > eb_g_limits.max_file_bytes) return eb_fail_msg(EB_E_LIMIT, "file too large");
    return EB_OK;
}

static char* to_utf8(const wchar_t* w)
{
    int cap = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, -1, NULL, 0, NULL, NULL);
    char* s = cap > 0 ? malloc((size_t)cap) : NULL;
    if (s && WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, -1, s, cap, NULL, NULL) <= 0) {
        free(s);
        s = NULL;
    }
    return s;
}

eb_status eb_open(const wchar_t* path, eb_doc** out)
{
    fz_document* fdoc = NULL;
    fz_context* ctx = NULL;
    char* utf8 = NULL;
    eb_status st = EB_OK;
    if (!out) return eb_fail_msg(EB_E_ARG, "null out");
    *out = NULL;
    if (!g_base) return eb_fail_msg(EB_E_ARG, "eb_init not called");
    if (!path || wcsnlen(path, EB_MAX_PATH_CHARS + 1) > EB_MAX_PATH_CHARS) return eb_fail_msg(EB_E_ARG, "bad path");
    st = check_file(path);
    if (st != EB_OK) return st;
    utf8 = to_utf8(path);
    ctx = fz_clone_context(g_base);
    if (!utf8 || !ctx) {
        free(utf8);
        fz_drop_context(ctx);
        return eb_fail_msg(EB_E_NOMEM, "out of memory");
    }
    fz_var(fdoc);
    fz_try(ctx) fdoc = fz_open_document(ctx, utf8);
    fz_catch(ctx) st = eb_fail(ctx);
    free(utf8);
    if (st != EB_OK) {
        fz_drop_context(ctx);
        return st;
    }
    return adopt(ctx, fdoc, out);
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
    if (!g_base) return eb_fail_msg(EB_E_ARG, "eb_init not called");
    if (!data || !size || !magic) return eb_fail_msg(EB_E_ARG, "bad memory arguments");
    if (size > eb_g_limits.max_file_bytes) return eb_fail_msg(EB_E_LIMIT, "data too large");
    ctx = fz_clone_context(g_base);
    if (!ctx) return eb_fail_msg(EB_E_NOMEM, "out of memory");
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
    if (st != EB_OK) {
        fz_drop_context(ctx);
        return st;
    }
    return adopt(ctx, fdoc, out);
}

void eb_close(eb_doc* doc)
{
    if (!doc) return;
    fz_drop_document(doc->ctx, doc->doc);
    fz_drop_context(doc->ctx);
    free(doc);
}

int32_t eb_is_reflowable(eb_doc* doc) { return doc ? doc->reflowable : 0; }

static int layout_args_ok(float w, float h, float em)
{
    return isfinite(w) && isfinite(h) && isfinite(em) && w >= 1.0f && h >= 1.0f && em >= 1.0f &&
           w <= EB_MAX_LAYOUT_PT && h <= EB_MAX_LAYOUT_PT && em <= 1000.0f;
}

eb_status eb_layout(eb_doc* doc, float width_pt, float height_pt, float em_pt, const char* user_css)
{
    eb_status st = EB_OK;
    int n = 0;
    if (!doc) return eb_fail_msg(EB_E_ARG, "null doc");
    if (!layout_args_ok(width_pt, height_pt, em_pt)) return eb_fail_msg(EB_E_ARG, "bad layout size");
    if (user_css && strnlen(user_css, EB_MAX_CSS_BYTES + 1) > EB_MAX_CSS_BYTES) return eb_fail_msg(EB_E_LIMIT, "css too large");
    if (!doc->reflowable) return EB_OK;
    fz_try(doc->ctx)
    {
        fz_style_document(doc->ctx, doc->doc, 1, user_css);
        fz_layout_document(doc->ctx, doc->doc, width_pt, height_pt, em_pt);
        n = fz_count_pages(doc->ctx, doc->doc);
    }
    fz_catch(doc->ctx) st = eb_fail(doc->ctx);
    if (st == EB_OK && (n < 0 || (uint32_t)n > eb_g_limits.max_pages)) st = eb_fail_msg(EB_E_LIMIT, "too many pages");
    return st;
}

eb_status eb_page_count(eb_doc* doc, int32_t* out)
{
    eb_status st = EB_OK;
    if (!doc || !out) return eb_fail_msg(EB_E_ARG, "null argument");
    fz_try(doc->ctx) * out = fz_count_pages(doc->ctx, doc->doc);
    fz_catch(doc->ctx) st = eb_fail(doc->ctx);
    return st;
}

eb_status eb_check_page(eb_doc* doc, int32_t page)
{
    int n = 0;
    fz_try(doc->ctx) n = fz_count_pages(doc->ctx, doc->doc);
    fz_catch(doc->ctx) return eb_fail(doc->ctx);
    if (page < 0 || page >= n) return eb_fail_msg(EB_E_RANGE, "page out of range");
    return EB_OK;
}

eb_status eb_page_size(eb_doc* doc, int32_t page, float* width_pt, float* height_pt)
{
    eb_status st = EB_OK;
    fz_page* pg = NULL;
    if (!doc || !width_pt || !height_pt) return eb_fail_msg(EB_E_ARG, "null argument");
    st = eb_check_page(doc, page);
    if (st != EB_OK) return st;
    fz_var(pg);
    fz_try(doc->ctx)
    {
        pg = fz_load_page(doc->ctx, doc->doc, page);
        fz_rect r = fz_bound_page(doc->ctx, pg);
        *width_pt = r.x1 - r.x0;
        *height_pt = r.y1 - r.y0;
    }
    fz_always(doc->ctx) fz_drop_page(doc->ctx, pg);
    fz_catch(doc->ctx) st = eb_fail(doc->ctx);
    return st;
}

eb_status eb_bookmark_of_page(eb_doc* doc, int32_t page, eb_bookmark* out)
{
    eb_status st = EB_OK;
    if (!doc || !out) return eb_fail_msg(EB_E_ARG, "null argument");
    st = eb_check_page(doc, page);
    if (st != EB_OK) return st;
    fz_try(doc->ctx)
    {
        fz_location loc = fz_location_from_page_number(doc->ctx, doc->doc, page);
        *out = (eb_bookmark)fz_make_bookmark(doc->ctx, doc->doc, loc);
    }
    fz_catch(doc->ctx) st = eb_fail(doc->ctx);
    return st;
}

eb_status eb_page_of_bookmark(eb_doc* doc, eb_bookmark mark, int32_t* page)
{
    eb_status st = EB_OK;
    int n = -1;
    if (!doc || !page) return eb_fail_msg(EB_E_ARG, "null argument");
    fz_try(doc->ctx)
    {
        fz_location loc = fz_lookup_bookmark(doc->ctx, doc->doc, (fz_bookmark)mark);
        n = fz_page_number_from_location(doc->ctx, doc->doc, loc);
    }
    fz_catch(doc->ctx) return eb_fail(doc->ctx);
    if (n < 0) return eb_fail_msg(EB_E_RANGE, "bookmark does not resolve to a page");
    *page = n;
    return st;
}

static const char* meta_key(const char* key)
{
    if (!strcmp(key, "title")) return FZ_META_INFO_TITLE;
    if (!strcmp(key, "author")) return FZ_META_INFO_AUTHOR;
    if (!strcmp(key, "format")) return FZ_META_FORMAT;
    return NULL;
}

eb_status eb_meta(eb_doc* doc, const char* key, char* buf, size_t cap, size_t* needed)
{
    char tmp[EB_MAX_META_BYTES];
    const char* mk = NULL;
    int n = -1;
    eb_status st = EB_OK;
    if (!doc || !key || !buf || !needed) return eb_fail_msg(EB_E_ARG, "null argument");
    mk = meta_key(key);
    if (!mk) return eb_fail_msg(EB_E_RANGE, "metadata key not available");
    fz_try(doc->ctx) n = fz_lookup_metadata(doc->ctx, doc->doc, mk, tmp, sizeof tmp);
    fz_catch(doc->ctx) return eb_fail(doc->ctx);
    if (n <= 1) return eb_fail_msg(EB_E_RANGE, "metadata missing");
    tmp[sizeof tmp - 1] = '\0';
    *needed = (size_t)n;
    if (cap > 0) strncpy_s(buf, cap, tmp, _TRUNCATE);
    return st;
}
