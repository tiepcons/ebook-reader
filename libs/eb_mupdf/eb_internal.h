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

struct eb_doc {
    fz_context* ctx;
    fz_document* doc;
    FILE* file;
    int reflowable;
    int laid_out;
    fz_cookie cookie;
    eb_mobi_info mobi;
};

extern eb_limits eb_g_limits;

/* Maps the MuPDF error just caught on ctx to an eb_status and records its message for eb_last_error. */
eb_status eb_fail(fz_context* ctx);
eb_status eb_fail_msg(eb_status s, const char* msg);

/* Bookmark encoding (see eb_mupdf.h); call inside fz_try. */
uint64_t eb_encode_bookmark(eb_doc* doc, fz_location loc);

/* Page access helpers: reflowable documents must have been laid out first (EB_E_ARG otherwise). */
eb_status eb_require_layout(eb_doc* doc);
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

#endif
