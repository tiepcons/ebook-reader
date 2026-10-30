#include "eb_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define EB_LAZY_MAX_CHAPTERS 20000
#define EB_LAZY_MAX_URI 4096
#define EB_BM_CHAPTER_MASK 0x7FFFFFFFu

void eb_lazy_free(eb_doc* doc)
{
    free(doc->ch.count);
    free(doc->ch.exact);
    doc->ch = (eb_chapters){0};
    doc->lazy = 0;
}

/* Average of the chapters laid out so far, never more than what keeps the whole book within the page limit. */
static int32_t estimate_of(const eb_chapters* c)
{
    int64_t e = 1;
    const int64_t unknown = c->n - c->exact_n;
    if (c->exact_n > 0) e = (c->exact_sum + c->exact_n / 2) / c->exact_n;
    if (unknown > 0 && e * unknown > (int64_t)eb_g_limits.max_pages - c->exact_sum) e = ((int64_t)eb_g_limits.max_pages - c->exact_sum) / unknown;
    return e < 1 ? 1 : (int32_t)e;
}

static void set_estimates(eb_chapters* c)
{
    c->estimate = estimate_of(c);
    for (int32_t i = 0; i < c->n; ++i) {
        if (!c->exact[i]) c->count[i] = c->estimate;
    }
}

static int64_t total_pages(const eb_chapters* c)
{
    return c->exact_sum + (int64_t)c->estimate * (c->n - c->exact_n);
}

int64_t eb_lazy_total(const eb_doc* doc)
{
    return total_pages(&doc->ch);
}

static int locate(const eb_chapters* c, int32_t page, int* chapter, int* in_chapter)
{
    int64_t start = 0;
    for (int32_t i = 0; i < c->n; ++i) {
        if (page < start + c->count[i]) {
            *chapter = i;
            *in_chapter = (int)(page - start);
            return 1;
        }
        start += c->count[i];
    }
    return 0;
}

static int32_t start_of(const eb_chapters* c, int chapter)
{
    int64_t start = 0;
    for (int i = 0; i < chapter; ++i) start += c->count[i];
    return (int32_t)start;
}

/* Lays out a chapter that is not exact yet and renumbers; the generation changes when any chapter's count changed. */
static void lay_out(eb_doc* d, int chapter)
{
    eb_chapters* c = &d->ch;
    int32_t was = 0, was_estimate = 0, now = 0;
    if (chapter < 0 || chapter >= c->n) fz_throw(d->ctx, FZ_ERROR_ARGUMENT, "chapter out of range");
    if (c->exact[chapter]) return;
    was = c->count[chapter];
    was_estimate = c->estimate;
    now = fz_count_chapter_pages(d->ctx, d->doc, chapter);
    if (now < 0 || (int64_t)now + c->exact_sum + (c->n - c->exact_n - 1) > (int64_t)eb_g_limits.max_pages)
        fz_throw(d->ctx, FZ_ERROR_LIMIT, "too many pages");
    c->exact[chapter] = 1;
    c->exact_n++;
    c->exact_sum += now;
    c->count[chapter] = now;
    set_estimates(c);
    if (now != was || (c->estimate != was_estimate && c->exact_n < c->n)) d->generation++;
}

int eb_chapter_pages(eb_doc* doc, int chapter)
{
    if (!doc->lazy) return fz_count_chapter_pages(doc->ctx, doc->doc, chapter);
    lay_out(doc, chapter);
    return doc->ch.count[chapter];
}

static int map_fraction(int in_chapter, int32_t was, int32_t now)
{
    double v = 0.0;
    if (now <= 0 || was <= 0) return 0;
    v = floor((double)in_chapter / (double)was * (double)now + 0.5);
    return v < 0.0 ? 0 : v > (double)(now - 1) ? now - 1 : (int)v;
}

fz_location eb_location_of_page(eb_doc* doc, int32_t page)
{
    eb_chapters* c = &doc->ch;
    int chapter = 0, in_chapter = 0;
    if (!doc->lazy) return fz_location_from_page_number(doc->ctx, doc->doc, page);
    if (page < 0 || !locate(c, page, &chapter, &in_chapter)) fz_throw(doc->ctx, FZ_ERROR_ARGUMENT, "page out of range");
    if (!c->exact[chapter]) {
        const int32_t was = c->count[chapter];
        lay_out(doc, chapter);
        in_chapter = map_fraction(in_chapter, was, c->count[chapter]);
    }
    return fz_make_location(chapter, in_chapter);
}

int eb_page_of_location(eb_doc* doc, fz_location loc)
{
    if (!doc->lazy) return fz_page_number_from_location(doc->ctx, doc->doc, loc);
    if (loc.chapter < 0 || loc.chapter >= doc->ch.n || loc.page < 0) return -1;
    if (doc->ch.exact[loc.chapter] ? loc.page >= doc->ch.count[loc.chapter] : loc.page != 0) return -1;
    return start_of(&doc->ch, loc.chapter) + loc.page;
}

fz_page* eb_load_page(eb_doc* doc, int32_t page)
{
    fz_location loc;
    if (!doc->lazy) return fz_load_page(doc->ctx, doc->doc, page);
    loc = eb_location_of_page(doc, page);
    return fz_load_chapter_page(doc->ctx, doc->doc, loc.chapter, loc.page);
}

/* A link into a chapter that is not laid out yet resolves to the start of that chapter: finding the anchor would lay it out. */
fz_location eb_resolve_uri(eb_doc* doc, const char* uri)
{
    char head[EB_LAZY_MAX_URI + 1];
    const char* hash = doc->lazy ? strchr(uri, '#') : NULL;
    if (hash && (size_t)(hash - uri) <= EB_LAZY_MAX_URI) {
        fz_location loc;
        memcpy(head, uri, (size_t)(hash - uri));
        head[hash - uri] = '\0';
        loc = fz_resolve_link(doc->ctx, doc->doc, head, NULL, NULL);
        if (loc.chapter >= 0 && loc.chapter < doc->ch.n && !doc->ch.exact[loc.chapter]) return loc;
    }
    return fz_resolve_link(doc->ctx, doc->doc, uri, NULL, NULL);
}

static void build_table(eb_doc* d, int32_t n)
{
    eb_chapters* c = &d->ch;
    c->n = n;
    c->count = calloc((size_t)n, sizeof *c->count);
    c->exact = calloc((size_t)n, sizeof *c->exact);
    if (!c->count || !c->exact) eb_throw_oom(d->ctx);
    eb_known_counts(d, c->count, n);
    for (int32_t i = 0; i < n; ++i) {
        if (c->count[i] >= 1 && (uint32_t)c->count[i] <= eb_g_limits.max_pages) {
            c->exact[i] = 1;
            c->exact_n++;
            c->exact_sum += c->count[i];
        }
    }
    set_estimates(c);
    d->lazy = 1;
}

eb_status eb_layout_lazy(eb_doc* doc, float width_pt, float height_pt, float em_pt, const char* user_css)
{
    eb_status st = eb_layout_begin(doc, width_pt, height_pt, em_pt, user_css);
    int32_t n = 0;
    if (st != EB_OK || !doc->reflowable) return st;
    fz_try(doc->ctx)
    {
        const int chapters = fz_document_supports_accelerator(doc->ctx, doc->doc) ? fz_count_chapters(doc->ctx, doc->doc) : 0;
        if (chapters > 1 && chapters <= EB_LAZY_MAX_CHAPTERS) build_table(doc, chapters);
    }
    fz_catch(doc->ctx)
    {
        eb_lazy_free(doc);
        st = eb_fail(doc->ctx);
    }
    if (st != EB_OK) return st;
    return eb_count_pages(doc, &n);
}

static int next_unlaid(const eb_chapters* c, eb_bookmark focus)
{
    int32_t from = (int32_t)((uint32_t)(focus >> 32) & EB_BM_CHAPTER_MASK);
    if (from >= c->n) from = c->n - 1;
    for (int32_t i = from; i < c->n; ++i) {
        if (!c->exact[i]) return i;
    }
    for (int32_t i = from - 1; i >= 0; --i) {
        if (!c->exact[i]) return i;
    }
    return -1;
}

eb_status eb_layout_step(eb_doc* doc, eb_bookmark focus, int32_t* complete)
{
    eb_status st = EB_OK;
    if (!doc || !complete) return eb_fail_msg(EB_E_ARG, "null argument");
    *complete = 1;
    st = require_layout(doc);
    if (st != EB_OK || !doc->lazy) return st;
    eb_begin_call(doc);
    fz_try(doc->ctx)
    {
        const int next = next_unlaid(&doc->ch, focus);
        if (next >= 0) lay_out(doc, next);
        *complete = doc->ch.exact_n == doc->ch.n;
        eb_check_abort(doc);
    }
    fz_catch(doc->ctx)
    {
        *complete = 0;
        st = eb_fail(doc->ctx);
    }
    return st;
}

uint32_t eb_layout_generation(eb_doc* doc)
{
    return doc ? doc->generation : 0;
}
