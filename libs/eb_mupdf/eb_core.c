#include "eb_internal.h"

#include <math.h>
#include <string.h>
#include <windows.h>

#define EB_MAX_PATH_CHARS 32767
#define EB_MAX_CSS_BYTES (1u << 20)
#define EB_MAX_LAYOUT_PT 100000.0f
#define EB_MAX_META_BYTES 4096
#define EB_MAX_BITMAP_PX (256u << 20)

eb_limits eb_g_limits;
static fz_context* g_base;
static CRITICAL_SECTION g_cs[FZ_LOCK_MAX];
static SRWLOCK g_lock = SRWLOCK_INIT;
static volatile LONG g_open_docs;
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
    case FZ_ERROR_ABORT: return eb_fail_msg(EB_E_ABORTED, msg);
    case FZ_ERROR_LIBRARY:
    case FZ_ERROR_UNSUPPORTED:
    case FZ_ERROR_FORMAT:
    case FZ_ERROR_SYNTAX: return eb_fail_msg(EB_E_FORMAT, msg);
    case FZ_ERROR_SYSTEM: return eb_fail_msg(eb_take_oom() ? EB_E_NOMEM : EB_E_OPEN, msg);
    default: return eb_fail_msg(EB_E_INTERNAL, msg);
    }
}

int32_t eb_abi_version(void) { return EB_ABI_VERSION; }
const char* eb_engine_version(void) { return FZ_VERSION; }

const char* eb_last_error(void) { return t_err; }

const char* eb_status_text(eb_status s)
{
    static const char* const text[] = {"ok",           "invalid argument", "cannot read file", "unsupported or corrupt document",
                                       "encrypted document", "limit exceeded", "out of memory", "out of range",
                                       "cancelled",    "internal error"};
    return (unsigned)s < sizeof text / sizeof text[0] ? text[s] : "unknown status";
}

static void apply_limits(const eb_limits* in)
{
    eb_g_limits = (eb_limits){2ull << 30, 100000, 64u << 20, (size_t)128 << 20, 1ull << 30};
    if (in) {
        if (in->max_file_bytes) eb_g_limits.max_file_bytes = in->max_file_bytes;
        if (in->max_pages) eb_g_limits.max_pages = in->max_pages;
        if (in->max_bitmap_px) eb_g_limits.max_bitmap_px = in->max_bitmap_px;
        if (in->store_bytes) eb_g_limits.store_bytes = in->store_bytes;
        if (in->max_heap_bytes) eb_g_limits.max_heap_bytes = in->max_heap_bytes;
    }
    if (eb_g_limits.max_bitmap_px > EB_MAX_BITMAP_PX) eb_g_limits.max_bitmap_px = EB_MAX_BITMAP_PX;
}

static void delete_locks(void)
{
    for (int i = 0; i < FZ_LOCK_MAX; ++i) DeleteCriticalSection(&g_cs[i]);
}

eb_status eb_init(const eb_limits* limits)
{
    eb_status st = EB_OK;
    AcquireSRWLockExclusive(&g_lock);
    if (g_base) {
        ReleaseSRWLockExclusive(&g_lock);
        return eb_fail_msg(EB_E_ARG, "already initialised");
    }
    apply_limits(limits);
    for (int i = 0; i < FZ_LOCK_MAX; ++i) InitializeCriticalSection(&g_cs[i]);
    g_locks = (fz_locks_context){g_cs, lock_cb, unlock_cb};
    g_base = fz_new_context(eb_alloc_context(eb_g_limits.max_heap_bytes), &g_locks, eb_g_limits.store_bytes);
    if (!g_base) st = eb_fail_msg(EB_E_NOMEM, "cannot create context");
    if (g_base) {
        fz_set_warning_callback(g_base, silent_cb, NULL);
        fz_set_error_callback(g_base, silent_cb, NULL);
        fz_disable_icc(g_base);
        fz_try(g_base) fz_register_document_handlers(g_base);
        fz_catch(g_base) st = eb_fail(g_base);
    }
    if (st != EB_OK) {
        fz_drop_context(g_base);
        g_base = NULL;
        delete_locks();
    }
    ReleaseSRWLockExclusive(&g_lock);
    return st;
}

/* While any eb_doc is open the engine must stay alive (its contexts share the base locks and allocator), so
 * eb_shutdown then returns without doing anything; the host must close all documents first. */
void eb_shutdown(void)
{
    AcquireSRWLockExclusive(&g_lock);
    if (g_base && InterlockedCompareExchange(&g_open_docs, 0, 0) == 0) {
        fz_drop_context(g_base);
        g_base = NULL;
        delete_locks();
    }
    ReleaseSRWLockExclusive(&g_lock);
}

fz_context* eb_acquire_ctx(void)
{
    fz_context* ctx = NULL;
    AcquireSRWLockShared(&g_lock);
    if (g_base) {
        ctx = fz_clone_context(g_base);
        if (ctx) {
            fz_disable_icc(ctx);
            InterlockedIncrement(&g_open_docs);
        } else {
            eb_note_oom();
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return ctx;
}

void eb_release_ctx(fz_context* ctx)
{
    fz_drop_context(ctx);
    InterlockedDecrement(&g_open_docs);
}

static eb_status require_layout(eb_doc* doc)
{
    if (doc->reflowable && !doc->laid_out) return eb_fail_msg(EB_E_ARG, "eb_layout not called");
    return EB_OK;
}

eb_status eb_count_pages(eb_doc* doc, int32_t* out)
{
    int n = 0;
    eb_status st = require_layout(doc);
    if (st != EB_OK) return st;
    fz_try(doc->ctx) n = fz_count_pages(doc->ctx, doc->doc);
    fz_catch(doc->ctx) return eb_fail(doc->ctx);
    if (n < 0 || (uint32_t)n > eb_g_limits.max_pages) return eb_fail_msg(EB_E_LIMIT, "too many pages");
    *out = n;
    return EB_OK;
}

eb_status eb_check_page(eb_doc* doc, int32_t page)
{
    int32_t n = 0;
    eb_status st = eb_count_pages(doc, &n);
    if (st != EB_OK) return st;
    if (page < 0 || page >= n) return eb_fail_msg(EB_E_RANGE, "page out of range");
    return EB_OK;
}

eb_status eb_require_layout(eb_doc* doc) { return require_layout(doc); }

int32_t eb_is_reflowable(eb_doc* doc) { return doc ? doc->reflowable : 0; }

static int layout_args_ok(float w, float h, float em)
{
    return isfinite(w) && isfinite(h) && isfinite(em) && w >= 1.0f && h >= 1.0f && em >= 1.0f &&
           w <= EB_MAX_LAYOUT_PT && h <= EB_MAX_LAYOUT_PT && em <= 1000.0f;
}

eb_status eb_layout(eb_doc* doc, float width_pt, float height_pt, float em_pt, const char* user_css)
{
    eb_status st = EB_OK;
    int32_t n = 0;
    if (!doc) return eb_fail_msg(EB_E_ARG, "null doc");
    if (!layout_args_ok(width_pt, height_pt, em_pt)) return eb_fail_msg(EB_E_ARG, "bad layout size");
    if (user_css && strnlen(user_css, EB_MAX_CSS_BYTES + 1) > EB_MAX_CSS_BYTES) return eb_fail_msg(EB_E_LIMIT, "css too large");
    if (!doc->reflowable) return EB_OK;
    fz_try(doc->ctx)
    {
        fz_style_document(doc->ctx, doc->doc, 1, user_css);
        fz_layout_document(doc->ctx, doc->doc, width_pt, height_pt, em_pt);
    }
    fz_catch(doc->ctx) st = eb_fail(doc->ctx);
    if (st != EB_OK) return st;
    doc->laid_out = 1;
    return eb_count_pages(doc, &n);
}

eb_status eb_page_count(eb_doc* doc, int32_t* out)
{
    if (!doc || !out) return eb_fail_msg(EB_E_ARG, "null argument");
    return eb_count_pages(doc, out);
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
        *out = eb_encode_bookmark(doc, loc);
    }
    fz_catch(doc->ctx) st = eb_fail(doc->ctx);
    return st;
}

#define EB_BM_FRAC_SCALE 4294967296.0
#define EB_BM_CHAPTER_MASK 0x7FFFFFFFu

/* Must run inside fz_try. Reflowable: chapter and fraction of the chapter survive layout changes and restarts;
 * fixed layout: exact page index. Bit 63 stays clear so signed ordering follows reading order. */
uint64_t eb_encode_bookmark(eb_doc* doc, fz_location loc)
{
    fz_context* ctx = doc->ctx;
    double frac = 0.0;
    int count = 0;
    if (!doc->reflowable) return (uint64_t)(uint32_t)fz_page_number_from_location(ctx, doc->doc, loc);
    if (loc.chapter < 0 || loc.page < 0) fz_throw(ctx, FZ_ERROR_ARGUMENT, "unresolved location");
    count = fz_count_chapter_pages(ctx, doc->doc, loc.chapter);
    if (count > 0) frac = floor((double)loc.page / (double)count * EB_BM_FRAC_SCALE + 0.5);
    if (frac > EB_BM_FRAC_SCALE - 1.0) frac = EB_BM_FRAC_SCALE - 1.0;
    return ((uint64_t)((uint32_t)loc.chapter & EB_BM_CHAPTER_MASK) << 32) | (uint64_t)frac;
}

static int decode_bookmark(eb_doc* doc, eb_bookmark mark)
{
    fz_context* ctx = doc->ctx;
    int chapter = 0, count = 0, in_chapter = 0;
    if (!doc->reflowable) return (int)(uint32_t)(mark & 0xFFFFFFFFu);
    chapter = (int)((uint32_t)(mark >> 32) & EB_BM_CHAPTER_MASK);
    if (chapter >= fz_count_chapters(ctx, doc->doc)) chapter = fz_count_chapters(ctx, doc->doc) - 1;
    if (chapter < 0) return -1;
    count = fz_count_chapter_pages(ctx, doc->doc, chapter);
    if (count <= 0) return -1;
    in_chapter = (int)floor((double)(uint32_t)(mark & 0xFFFFFFFFu) / EB_BM_FRAC_SCALE * (double)count + 0.5);
    if (in_chapter > count - 1) in_chapter = count - 1;
    return fz_page_number_from_location(ctx, doc->doc, (fz_location){chapter, in_chapter});
}

eb_status eb_page_of_bookmark(eb_doc* doc, eb_bookmark mark, int32_t* page)
{
    int32_t count = 0;
    int n = -1;
    eb_status st = EB_OK;
    if (!doc || !page) return eb_fail_msg(EB_E_ARG, "null argument");
    st = eb_count_pages(doc, &count);
    if (st != EB_OK) return st;
    fz_try(doc->ctx) n = decode_bookmark(doc, mark);
    fz_catch(doc->ctx) return eb_fail(doc->ctx);
    if (n < 0 || n >= count) return eb_fail_msg(EB_E_RANGE, "bookmark does not resolve to a page");
    *page = n;
    return EB_OK;
}

static const char* meta_key(const char* key)
{
    if (!strcmp(key, "title")) return FZ_META_INFO_TITLE;
    if (!strcmp(key, "author")) return FZ_META_INFO_AUTHOR;
    if (!strcmp(key, "format")) return FZ_META_FORMAT;
    return NULL;
}

static const char* mobi_value(const eb_mobi_info* info, const char* key)
{
    const char* v = NULL;
    if (!strcmp(key, "title")) v = info->title;
    else if (!strcmp(key, "author")) v = info->author;
    else if (!strcmp(key, "language")) v = info->language;
    else if (!strcmp(key, "format")) v = info->format;
    return v && *v ? v : NULL;
}

static eb_status emit_meta(const char* value, char* buf, size_t cap, size_t* needed)
{
    *needed = strlen(value) + 1;
    if (cap > 0) strncpy_s(buf, cap, value, _TRUNCATE);
    return EB_OK;
}

/* Order: libmobi values, MuPDF's own keys, then the EPUB/FB2 metadata read by eb_embed.c. */
eb_status eb_meta(eb_doc* doc, const char* key, char* buf, size_t cap, size_t* needed)
{
    char tmp[EB_MAX_META_BYTES];
    const char* value = NULL;
    const char* mk = NULL;
    int n = -1;
    if (!doc || !key || !buf || !needed) return eb_fail_msg(EB_E_ARG, "null argument");
    value = mobi_value(&doc->mobi, key);
    if (value) return emit_meta(value, buf, cap, needed);
    mk = meta_key(key);
    if (mk) {
        fz_try(doc->ctx) n = fz_lookup_metadata(doc->ctx, doc->doc, mk, tmp, sizeof tmp);
        fz_catch(doc->ctx) return eb_fail(doc->ctx);
    }
    if (n > 1) {
        tmp[sizeof tmp - 1] = '\0';
        *needed = (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp;
        if (cap > 0) strncpy_s(buf, cap, tmp, _TRUNCATE);
        return EB_OK;
    }
    if (!strcmp(key, "title") || !strcmp(key, "author") || !strcmp(key, "language")) {
        eb_embed_probe(doc, 0);
        value = mobi_value(&doc->mobi, key);
        if (value) return emit_meta(value, buf, cap, needed);
    }
    return eb_fail_msg(EB_E_RANGE, "metadata missing");
}
