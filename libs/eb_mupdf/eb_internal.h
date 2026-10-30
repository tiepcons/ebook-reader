#ifndef EB_INTERNAL_H
#define EB_INTERNAL_H

#include "eb_mupdf.h"

#include <mupdf/fitz.h>
#include <stdio.h>

#define EB_MOBI_MAX_INPUT ((size_t)256 << 20)
#define EB_MOBI_MAX_EPUB ((size_t)256 << 20)

/* Metadata and cover pulled out of a Kindle/MOBI book by libmobi; strings are malloc'd, UTF-8, may be NULL. */
typedef struct eb_mobi_info {
    char* title;
    char* author;
    char* language;
    const char* format;
    uint8_t* cover;
    size_t cover_size;
    int rejected; /* set when a structural pre-check refused the book: no MuPDF fallback */
} eb_mobi_info;

/* Identity of a book file taken from its open handle; the layout cache is bound to it. */
typedef struct eb_stamp {
    uint64_t size;
    uint64_t mtime; /* FILETIME */
    uint64_t volume, index; /* volume serial and file index: the same path replaced by another file is a different book */
    int valid;
} eb_stamp;

typedef struct eb_accel {
    const uint8_t* data;
    size_t size;
} eb_accel;

/* Per-chapter page counts of a lazily laid out book (eb_lazy.c): the exact count, or the shared estimate while exact[i] is 0. */
typedef struct eb_chapters {
    int32_t n, exact_n, estimate;
    int32_t* count;
    uint8_t* exact;
    int64_t exact_sum;
} eb_chapters;

struct eb_doc {
    fz_context* ctx;
    eb_stamp stamp; /* valid only for documents opened from a file */
    fz_document* doc;
    FILE* file;
    int reflowable;
    int laid_out;
    int lazy;            /* ch is valid: page numbers come from the chapter table, not from fz_count_pages */
    uint32_t generation; /* changes whenever page numbers may have changed (eb_layout_generation) */
    eb_chapters ch;
    fz_cookie cookie;
    eb_mobi_info mobi; /* libmobi books fill it at open; EPUB/FB2 lazily via eb_embed_probe */
    fz_buffer* src;    /* kept bytes of memory-opened documents, for eb_embed.c */
    fz_buffer* cover_buf; /* embedded EPUB/FB2 cover bytes, released after the first successful cover render */
    int meta_probed, cover_probed;
    int font_loads; /* system-font loads charged to this document (eb_font.c), reached via the ctx user pointer */
};

extern eb_limits eb_g_limits;

static inline int is_sep(wchar_t c) { return c == L'\\' || c == L'/'; }

/* Maps the MuPDF error just caught on ctx to an eb_status and records its message for eb_last_error. */
eb_status eb_fail(fz_context* ctx);
eb_status eb_fail_msg(eb_status s, const char* msg);

/* Bookmark encoding (see eb_mupdf.h); call inside fz_try. */
uint64_t eb_encode_bookmark(eb_doc* doc, fz_location loc);

/* eb_core.c: validates the arguments, applies style and layout size, resets the numbering; no page is counted. */
eb_status eb_layout_begin(eb_doc* doc, float width_pt, float height_pt, float em_pt, const char* user_css);

/* eb_lazy.c (all but eb_lazy_free throw: call inside fz_try). In lazy mode a call that needs a chapter lays it out first. */
void eb_lazy_free(eb_doc* doc);
int64_t eb_lazy_total(const eb_doc* doc);
int eb_chapter_pages(eb_doc* doc, int chapter);
fz_location eb_location_of_page(eb_doc* doc, int32_t page);
int eb_page_of_location(eb_doc* doc, fz_location loc);
fz_page* eb_load_page(eb_doc* doc, int32_t page);
fz_location eb_resolve_uri(eb_doc* doc, const char* uri);

/* Page access helpers: reflowable documents must have been laid out first (EB_E_ARG otherwise). */
eb_status require_layout(eb_doc* doc);
eb_status eb_count_pages(eb_doc* doc, int32_t* out);
eb_status eb_check_page(eb_doc* doc, int32_t page);

/* Clones the base context and counts it as an open document; release with eb_release_ctx. NULL if not initialised. */
fz_context* eb_acquire_ctx(void);
void eb_release_ctx(fz_context* ctx);

/* Capped allocator; failures are remembered per thread so FZ_ERROR_SYSTEM can be told apart from I/O errors. */
const fz_alloc_context* eb_alloc_context(uint64_t cap);
void eb_note_oom(void);
int eb_take_oom(void);
void eb_throw_oom(fz_context* ctx);

/* Per-call cancellation: eb_begin_call resets the cookie, eb_check_abort throws FZ_ERROR_ABORT if it was set. */
void eb_begin_call(eb_doc* doc);
void eb_check_abort(eb_doc* doc);

/* eb_mobi.c: libmobi to in-memory EPUB. eb_mobi_sniff needs the first 68 bytes. */
int eb_mobi_sniff(const uint8_t* head, size_t size);
eb_status eb_mobi_convert(fz_context* ctx, const uint8_t* data, size_t size, fz_buffer** epub, eb_mobi_info* info);
void eb_mobi_info_free(eb_mobi_info* info);

/* eb_mobi_check.c: cheap structural checks on a loaded book (struct MOBIData is libmobi's), before it is parsed. */
struct MOBIData;
eb_status eb_mobi_precheck(const struct MOBIData* m, size_t file_size);

/* eb_cover.c: renders the embedded cover image (doc->mobi.cover) scaled so its longer side is max_side. */
eb_status eb_render_embedded_cover(eb_doc* doc, int32_t max_side, eb_bitmap** out);

/* eb_render.c helpers shared with eb_cover.c. eb_new_bitmap throws EB_E_LIMIT-class errors on ctx; the pixmap
 * aliases the bitmap's pixels (BGRA, alpha forced opaque by eb_make_opaque). */
eb_bitmap* eb_new_bitmap(fz_context* ctx, int w, int h);
fz_pixmap* eb_bitmap_pixmap(fz_context* ctx, eb_bitmap* bmp);
void eb_make_opaque(fz_pixmap* pix);

/* eb_font.c: drops the indexed font list and cached fonts; call before the base context is dropped. */
void eb_fonts_release(fz_context* ctx);

/* eb_cache.c: layout cache files. eb_stamp_of fills *st from the open stream (valid = 0 on failure). eb_cache_load
 * returns the validated MuPDF accelerator blob (malloc'd, free()) or NULL when the cache is absent, stale or damaged. */
void eb_stamp_of(FILE* f, eb_stamp* st);
uint8_t* eb_cache_load(const wchar_t* cache_path, const eb_stamp* stamp, size_t* size);

/* eb_cache.c: fills out[0..cap) with the page count MuPDF already knows for each chapter, -1 for the others. Throws. */
void eb_known_counts(eb_doc* doc, int32_t* out, int32_t cap);

/* eb_font.c: hash of the registered font files (path, size, mtime); 0 when none. Set once per eb_set_font_dirs. */
uint64_t eb_font_digest(void);

/* eb_embed.c: EPUB/FB2 language, fallback title/author and embedded cover, filled into doc->mobi on demand. */
int eb_embed_probe(eb_doc* doc, int want_cover); /* 1 only if eb_abort cancelled a cover probe */
char* eb_normalize_language(const char* s); /* lowercase primary subtag ("vi"), or NULL; malloc'd */

#endif
