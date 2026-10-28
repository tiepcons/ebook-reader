#include "eb_internal.h"

#include <math.h>
#include <string.h>

#define EB_MAX_TEXT_CHARS 4000000
#define EB_MAX_LINKS 4096
#define EB_MAX_TOC_ITEMS 100000
#define EB_MAX_TOC_DEPTH 64

typedef struct sink {
    int fill, over;
    float ox, oy; /* page bound origin: the renderer shifts it to (0,0) */
    size_t bytes;
    int32_t n;
    char* utf8;
    uint32_t* off;
    eb_rect* box;
} sink;

static void put(sink* s, int c, eb_rect r)
{
    char tmp[8];
    int len = 0;
    if (s->n >= EB_MAX_TEXT_CHARS) {
        s->over = 1;
        return;
    }
    len = fz_runetochar(tmp, (c > 0 && c <= 0x10FFFF && !(c >= 0xD800 && c <= 0xDFFF)) ? c : 0xFFFD);
    if (s->fill) {
        memcpy(s->utf8 + s->bytes, tmp, (size_t)len);
        s->off[s->n] = (uint32_t)s->bytes;
        s->box[s->n] = r;
    }
    s->bytes += (size_t)len;
    s->n++;
}

static void walk(const fz_stext_page* st, sink* s)
{
    int first = 1;
    eb_rect last = {0};
    for (const fz_stext_block* b = st->first_block; b; b = b->next) {
        if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
        for (const fz_stext_line* l = b->u.t.first_line; l; l = l->next) {
            if (!first) put(s, '\n', (eb_rect){last.x1, last.y0, last.x1, last.y1});
            first = 0;
            for (const fz_stext_char* ch = l->first_char; ch; ch = ch->next) {
                fz_rect q = fz_rect_from_quad(ch->quad);
                last = (eb_rect){q.x0 - s->ox, q.y0 - s->oy, q.x1 - s->ox, q.y1 - s->oy};
                put(s, ch->c, last);
            }
        }
    }
}

void eb_text_free(eb_text* t)
{
    if (!t) return;
    free(t->utf8);
    free(t->utf8_offset);
    free(t->box);
    free(t);
}

static void alloc_text(fz_context* ctx, eb_text* t, const sink* count)
{
    size_t n = (size_t)count->n;
    t->utf8 = malloc(count->bytes + 1);
    t->utf8_offset = malloc((n ? n : 1) * sizeof *t->utf8_offset);
    t->box = malloc((n ? n : 1) * sizeof *t->box);
    if (!t->utf8 || !t->utf8_offset || !t->box) eb_throw_oom(ctx);
}

static void extract(fz_context* ctx, const fz_stext_page* st, fz_rect bound, eb_text* t)
{
    sink s = {0};
    s.ox = bound.x0;
    s.oy = bound.y0;
    walk(st, &s);
    if (s.over) fz_throw(ctx, FZ_ERROR_LIMIT, "page text too large");
    alloc_text(ctx, t, &s);
    t->utf8_len = s.bytes;
    t->char_count = s.n;
    s.fill = 1;
    s.bytes = 0;
    s.n = 0;
    s.utf8 = t->utf8;
    s.off = t->utf8_offset;
    s.box = t->box;
    walk(st, &s);
    t->utf8[s.bytes] = '\0';
}

eb_status eb_page_text(eb_doc* doc, int32_t page, eb_text** out)
{
    fz_page* pg = NULL;
    fz_stext_page* st = NULL;
    eb_text* t = NULL;
    eb_status status = EB_OK;
    if (!doc || !out) return eb_fail_msg(EB_E_ARG, "null argument");
    *out = NULL;
    status = eb_check_page(doc, page);
    if (status != EB_OK) return status;
    t = calloc(1, sizeof *t);
    if (!t) return eb_fail_msg(EB_E_NOMEM, "out of memory");
    eb_begin_call(doc);
    fz_var(pg);
    fz_var(st);
    fz_try(doc->ctx)
    {
        fz_stext_options opts;
        fz_init_stext_options(doc->ctx, &opts);
        pg = fz_load_page(doc->ctx, doc->doc, page);
        st = fz_new_stext_page_from_page_with_cookie(doc->ctx, pg, &opts, &doc->cookie);
        eb_check_abort(doc);
        extract(doc->ctx, st, fz_bound_page(doc->ctx, pg), t);
    }
    fz_always(doc->ctx)
    {
        fz_drop_stext_page(doc->ctx, st);
        fz_drop_page(doc->ctx, pg);
    }
    fz_catch(doc->ctx) status = eb_fail(doc->ctx);
    if (status != EB_OK) eb_text_free(t);
    else *out = t;
    return status;
}

void eb_links_free(eb_link* links)
{
    free(links);
}

static int finite_rect(fz_rect r)
{
    return isfinite(r.x0) && isfinite(r.y0) && isfinite(r.x1) && isfinite(r.y1);
}

static int link_target(eb_doc* d, const fz_link* l, fz_rect bound, int32_t npages, eb_link* out)
{
    fz_location loc;
    fz_rect a = l->rect;
    if (!finite_rect(a)) return 0;
    a = fz_transform_rect(a, fz_translate(-bound.x0, -bound.y0));
    a = fz_intersect_rect(a, (fz_rect){0, 0, bound.x1 - bound.x0, bound.y1 - bound.y0});
    if (fz_is_empty_rect(a)) return 0;
    loc = fz_resolve_link(d->ctx, d->doc, l->uri, NULL, NULL);
    if (loc.chapter < 0 || loc.page < 0) return 0;
    out->page = fz_page_number_from_location(d->ctx, d->doc, loc);
    if (out->page < 0 || out->page >= npages) return 0;
    out->target = eb_encode_bookmark(d, loc);
    out->area = (eb_rect){a.x0, a.y0, a.x1, a.y1};
    return 1;
}

static int32_t count_links(const fz_link* head)
{
    int32_t n = 0;
    for (; head && n < EB_MAX_LINKS; head = head->next) n++;
    return n;
}

static int32_t collect_links(eb_doc* d, const fz_link* head, fz_rect bound, int32_t npages, eb_link* arr, int32_t cap)
{
    fz_context* ctx = d->ctx;
    int32_t n = 0;
    for (const fz_link* l = head; l && n < cap; l = l->next) {
        eb_check_abort(d);
        if (!l->uri || fz_is_external_link(ctx, l->uri)) continue;
        fz_try(ctx)
        {
            if (link_target(d, l, bound, npages, &arr[n])) n++;
        }
        fz_catch(ctx)
        {
            if (fz_caught(ctx) == FZ_ERROR_ABORT) fz_rethrow(ctx);
            fz_ignore_error(ctx);
        }
    }
    return n;
}

eb_status eb_page_links(eb_doc* doc, int32_t page, eb_link** links, int32_t* count)
{
    fz_page* pg = NULL;
    fz_link* head = NULL;
    eb_link* arr = NULL;
    int32_t n = 0, npages = 0;
    eb_status status = EB_OK;
    if (!doc || !links || !count) return eb_fail_msg(EB_E_ARG, "null argument");
    *links = NULL;
    *count = 0;
    status = eb_count_pages(doc, &npages);
    if (status == EB_OK) status = eb_check_page(doc, page);
    if (status != EB_OK) return status;
    eb_begin_call(doc);
    fz_var(pg);
    fz_var(head);
    fz_var(arr);
    fz_var(n);
    fz_try(doc->ctx)
    {
        int32_t cap;
        pg = fz_load_page(doc->ctx, doc->doc, page);
        head = fz_load_links(doc->ctx, pg);
        cap = count_links(head);
        arr = cap ? calloc((size_t)cap, sizeof *arr) : NULL;
        if (cap && !arr) eb_throw_oom(doc->ctx);
        n = collect_links(doc, head, fz_bound_page(doc->ctx, pg), npages, arr, cap);
    }
    fz_always(doc->ctx)
    {
        fz_drop_link(doc->ctx, head);
        fz_drop_page(doc->ctx, pg);
    }
    fz_catch(doc->ctx) status = eb_fail(doc->ctx);
    if (status != EB_OK || n == 0) {
        free(arr);
        return status;
    }
    *links = arr;
    *count = n;
    return EB_OK;
}

static int32_t count_items(const fz_outline* o, int depth)
{
    int32_t n = 0;
    for (; o && n < EB_MAX_TOC_ITEMS; o = o->next) {
        n++;
        if (o->down && depth + 1 < EB_MAX_TOC_DEPTH) n += count_items(o->down, depth + 1);
    }
    return n < EB_MAX_TOC_ITEMS ? n : EB_MAX_TOC_ITEMS;
}

static void resolve(eb_doc* d, const fz_outline* o, eb_toc_item* it)
{
    fz_context* ctx = d->ctx;
    fz_try(ctx)
    {
        fz_location loc = o->uri ? fz_resolve_link(ctx, d->doc, o->uri, NULL, NULL) : o->page;
        if (loc.chapter >= 0 && loc.page >= 0) {
            it->target = eb_encode_bookmark(d, loc);
            it->page = fz_page_number_from_location(ctx, d->doc, loc);
        }
    }
    fz_catch(ctx) fz_ignore_error(ctx);
}

static int fill_items(eb_doc* d, const fz_outline* o, int depth, eb_toc_item** cur, const eb_toc_item* end)
{
    for (; o && *cur < end; o = o->next) {
        eb_toc_item* it = (*cur)++;
        const char* title = o->title ? o->title : "";
        it->level = depth;
        it->page = -1;
        it->title = malloc(strlen(title) + 1);
        if (!it->title) return 0;
        memcpy(it->title, title, strlen(title) + 1);
        resolve(d, o, it);
        if (o->down && depth + 1 < EB_MAX_TOC_DEPTH && !fill_items(d, o->down, depth + 1, cur, end)) return 0;
    }
    return 1;
}

void eb_toc_free(eb_toc_item* items, int32_t count)
{
    for (int32_t i = 0; items && i < count; ++i) free(items[i].title);
    free(items);
}

eb_status eb_toc(eb_doc* doc, eb_toc_item** items, int32_t* count)
{
    fz_outline* ol = NULL;
    eb_toc_item* arr = NULL;
    eb_toc_item* cur = NULL;
    int32_t n = 0;
    eb_status st = EB_OK;
    if (!doc || !items || !count) return eb_fail_msg(EB_E_ARG, "null argument");
    *items = NULL;
    *count = 0;
    st = require_layout(doc);
    if (st != EB_OK) return st;
    fz_var(ol);
    fz_var(arr);
    fz_var(n);
    fz_try(doc->ctx)
    {
        ol = fz_load_outline(doc->ctx, doc->doc);
        n = count_items(ol, 0);
        arr = n ? calloc((size_t)n, sizeof *arr) : NULL;
        if (n && !arr) eb_throw_oom(doc->ctx);
        cur = arr;
        if (n && !fill_items(doc, ol, 0, &cur, arr + n)) eb_throw_oom(doc->ctx);
    }
    fz_always(doc->ctx) fz_drop_outline(doc->ctx, ol);
    fz_catch(doc->ctx) st = eb_fail(doc->ctx);
    if (st != EB_OK) {
        eb_toc_free(arr, n);
        return st;
    }
    *items = arr;
    *count = n;
    return EB_OK;
}
