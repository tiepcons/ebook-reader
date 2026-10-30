/* eb_mupdf: stable C ABI over MuPDF for Ebook Reader (PLAN.md D4, D9, D10).
 *
 * Contract:
 * - No function ever throws or aborts; every failure is an eb_status. MuPDF errors stay inside the DLL.
 * - Memory returned by the DLL is freed only by the matching eb_*_free function (separate static CRTs).
 * - Strings are UTF-8 unless the parameter is wchar_t (paths are UTF-16).
 * - An eb_doc may be used by one thread at a time; different eb_docs may be used concurrently.
 *   Exception: eb_abort may be called from any thread to cancel the doc's running render/text call.
 * - Positions that must survive re-layout (reading progress, bookmarks, TOC targets) are eb_bookmark values;
 *   page numbers are only valid until the next eb_layout call.
 */
#ifndef EB_MUPDF_H
#define EB_MUPDF_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#ifdef EB_MUPDF_BUILD
#define EB_API __declspec(dllexport)
#else
#define EB_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define EB_ABI_VERSION 5 /* v2: font directories, render_opts bg for reflow; v3: eb_page_links; v4: layout cache;
                            v5: progressive layout */

typedef enum eb_status {
    EB_OK = 0,
    EB_E_ARG,      /* invalid argument or call order */
    EB_E_OPEN,     /* file cannot be read */
    EB_E_FORMAT,   /* unsupported or corrupt document */
    EB_E_DRM,      /* encrypted / password protected */
    EB_E_LIMIT,    /* exceeds a configured limit */
    EB_E_NOMEM,
    EB_E_RANGE,    /* page / index out of range */
    EB_E_ABORTED,  /* cancelled by eb_abort */
    EB_E_INTERNAL
} eb_status;

typedef struct eb_doc eb_doc;
typedef uint64_t eb_bookmark; /* layout-independent position: (chapter<<32 | fraction-of-chapter) for reflowable docs, page index for fixed-layout; bit 63 is always 0; values are monotonic in reading order */

typedef struct eb_limits {
    uint64_t max_file_bytes;   /* reject larger files (default 2 GiB) */
    uint32_t max_pages;        /* reject documents with more pages (default 100000) */
    uint32_t max_bitmap_px;    /* max width*height of one rendered bitmap (default 64 Mpx) */
    size_t store_bytes;        /* MuPDF resource cache size (default 128 MiB) */
    uint64_t max_heap_bytes;   /* hard cap on all engine allocations (default 1 GiB) -> EB_E_NOMEM */
} eb_limits;

typedef struct eb_bitmap {
    int32_t width, height, stride; /* stride in bytes */
    uint8_t* bgra;                 /* premultiplied BGRA, top-down; ready for a D2D bitmap */
} eb_bitmap;

typedef struct eb_render_opts {
    float scale;          /* device pixels per point (96 dpi at 100% = 1.333f) */
    uint32_t fg_rgb;      /* text colour for tinting, 0xRRGGBB */
    uint32_t bg_rgb;      /* paper colour, 0xRRGGBB */
    int32_t tint;         /* all docs: 1 clears to white, renders, then maps black->fg and white->bg (instant theme
                             switch, no relayout; images are tinted too); 0 = reflowable docs are cleared to
                             bg_rgb and keep user_css colours, fixed-layout docs keep original colours */
} eb_render_opts;

typedef struct eb_rect {
    float x0, y0, x1, y1; /* points, page space */
} eb_rect;

typedef struct eb_text {
    char* utf8;            /* page text, '\n' between lines, NUL-terminated */
    size_t utf8_len;
    int32_t char_count;    /* number of code points */
    uint32_t* utf8_offset; /* [char_count] byte offset of each code point in utf8 */
    eb_rect* box;          /* [char_count] bounding box of each code point */
} eb_text;

typedef struct eb_link {
    eb_rect area;          /* hot area, points, page space */
    int32_t page;          /* target page in current layout */
    eb_bookmark target;
} eb_link;

typedef struct eb_toc_item {
    int32_t level;         /* 0 = top level */
    int32_t page;          /* page in current layout, -1 if unresolved */
    eb_bookmark target;
    char* title;
} eb_toc_item;

/* Library lifetime: call eb_init once before anything else, eb_shutdown after the last eb_close. */
EB_API int32_t eb_abi_version(void);
EB_API const char* eb_engine_version(void);
EB_API eb_status eb_init(const eb_limits* limits); /* NULL = defaults */
EB_API void eb_shutdown(void);
EB_API const char* eb_status_text(eb_status s);
EB_API const char* eb_last_error(void); /* detail of the last failure on the calling thread */

/* Fonts (v2). Register font directories searched, in order, when book CSS or user_css names a font family that
 * the document does not embed, and for glyph fallback (e.g. Vietnamese marks missing from a book font).
 * dirs: absolute UTF-16 paths; only .ttf/.otf/.ttc files are used (at most 512 files, each <= 32 MiB).
 * Call after eb_init and before opening documents; replaces any previous list; thread-safe. */
EB_API eb_status eb_set_font_dirs(const wchar_t* const* dirs, int32_t count);
/* Family names (UTF-8, de-duplicated, sorted) available from the registered dirs, for a font picker. */
EB_API eb_status eb_font_families(char*** names, int32_t* count);
EB_API void eb_font_families_free(char** names, int32_t count);

/* Documents */
EB_API eb_status eb_open(const wchar_t* path, eb_doc** out);
EB_API eb_status eb_open_memory(const void* data, size_t size, const char* magic, eb_doc** out); /* copies data */
EB_API void eb_close(eb_doc* doc);
/* Layout cache (v4) for reflowable books split into chapters (EPUB, and MOBI/AZW3/KF8 converted to EPUB): the file
 * holds the page count of every chapter for one layout (size, em, user_css), so eb_layout with the same values skips
 * laying out the whole book and only the chapters actually rendered are laid out.
 * The cache is bound to the book's file size and last-write time. A missing, stale, foreign or damaged cache (or one
 * larger than 1 MiB) is ignored: the open never fails because of it. cache_path NULL = eb_open. */
EB_API eb_status eb_open_cached(const wchar_t* path, const wchar_t* cache_path, eb_doc** out);
/* Writes the cache for the current layout, atomically (temp file + rename). Call after eb_layout succeeded.
 * Docs without chapter caching (PDF, CBZ, FB2, TXT, HTML, eb_open_memory) return EB_E_FORMAT and write nothing. */
EB_API eb_status eb_save_cache(eb_doc* doc, const wchar_t* cache_path);
/* Thread-safe. Makes the doc's current or next render/text call return EB_E_ABORTED; reset on the next call. */
EB_API void eb_abort(eb_doc* doc);
EB_API int32_t eb_is_reflowable(eb_doc* doc);
EB_API eb_status eb_layout(eb_doc* doc, float width_pt, float height_pt, float em_pt, const char* user_css);
/* Progressive layout (v5) for reflowable books split into chapters (EPUB, MOBI/AZW3/KF8); other docs behave as eb_layout.
 * eb_layout_lazy applies the layout without laying out the whole book. A chapter is laid out on demand by any call that
 * needs it (render, text, links, page size, bookmark <-> page) and by eb_layout_step. Until every chapter is laid out, the
 * page count of a chapter not laid out yet is an estimate (average of the chapters laid out so far), so eb_page_count and
 * the numbers of pages after such a chapter are provisional; eb_toc pages are provisional too.
 * Laying out a chapter whose count differed from its estimate renumbers the pages after it and increments
 * eb_layout_generation(). A page number passed to a call is read in the numbering current at the call; if the call lays
 * out the chapter holding that page, the page keeps its relative position in the chapter (same fraction, like a bookmark).
 * After any call, a caller holding page numbers compares eb_layout_generation() and re-resolves them from eb_bookmark values
 * (eb_page_of_bookmark lays out the bookmark's chapter first, so its result is exact for that chapter).
 * A matching layout cache (eb_open_cached) makes every count exact at once. eb_layout_lazy itself costs about one chapter. */
EB_API eb_status eb_layout_lazy(eb_doc* doc, float width_pt, float height_pt, float em_pt, const char* user_css);
/* Lays out one more chapter: the chapters after `near` in reading order first (nearest first), then the ones before it
 * (nearest first). *complete = 1 once every chapter is laid out and all page numbers are exact; then it is a cheap no-op.
 * Abortable like a render (eb_abort -> EB_E_ABORTED, nothing lost). */
EB_API eb_status eb_layout_step(eb_doc* doc, eb_bookmark near, int32_t* complete);
EB_API uint32_t eb_layout_generation(eb_doc* doc);
EB_API eb_status eb_page_count(eb_doc* doc, int32_t* out);
EB_API eb_status eb_page_size(eb_doc* doc, int32_t page, float* width_pt, float* height_pt);

/* Positions */
EB_API eb_status eb_bookmark_of_page(eb_doc* doc, int32_t page, eb_bookmark* out);
EB_API eb_status eb_page_of_bookmark(eb_doc* doc, eb_bookmark mark, int32_t* page);

/* Rendering */
EB_API eb_status eb_render_page(eb_doc* doc, int32_t page, const eb_render_opts* opts, eb_bitmap** out);
EB_API eb_status eb_render_cover(eb_doc* doc, int32_t max_side_px, eb_bitmap** out); /* embedded cover or page 1 */
EB_API void eb_bitmap_free(eb_bitmap* bmp);

/* Text, outline, metadata */
EB_API eb_status eb_page_text(eb_doc* doc, int32_t page, eb_text** out);
EB_API void eb_text_free(eb_text* text);
EB_API eb_status eb_toc(eb_doc* doc, eb_toc_item** items, int32_t* count);
EB_API void eb_toc_free(eb_toc_item* items, int32_t count);
/* v3. Internal links of a page (in-book TOC, footnotes). External URIs (http:, mailto:, ...) and unresolvable
 * targets are skipped. At most 4096 links; *links is NULL when count is 0. Abortable like eb_page_text. */
EB_API eb_status eb_page_links(eb_doc* doc, int32_t page, eb_link** links, int32_t* count);
EB_API void eb_links_free(eb_link* links);
/* key: "title", "author", "language", "format". Writes a NUL-terminated string, truncating to cap; *needed
 * receives the full size including NUL. Missing key -> EB_E_RANGE. */
EB_API eb_status eb_meta(eb_doc* doc, const char* key, char* buf, size_t cap, size_t* needed);

#ifdef __cplusplus
}
#endif
#endif
