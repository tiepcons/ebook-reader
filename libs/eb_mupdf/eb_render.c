#include "eb_internal.h"

#include <math.h>

#define EB_MAX_COVER_SIDE 8192
#define EB_MAX_SIDE_PX 32767.0
#define EB_PIXEL_OFFSET (((sizeof(eb_bitmap) + 15) / 16) * 16)
#define EB_MAX_BITMAP_SIDE 32767

typedef struct render_req {
    float scale;
    int32_t max_side;
    int tint;
    uint32_t fg, bg;
} render_req;

eb_bitmap* eb_new_bitmap(fz_context* ctx, int w, int h)
{
    eb_bitmap* bmp = NULL;
    if (w < 1 || h < 1 || w > EB_MAX_BITMAP_SIDE || h > EB_MAX_BITMAP_SIDE ||
        (double)w * (double)h > (double)eb_g_limits.max_bitmap_px)
        fz_throw(ctx, FZ_ERROR_LIMIT, "bitmap exceeds max_bitmap_px");
    bmp = malloc(EB_PIXEL_OFFSET + (size_t)w * (size_t)h * 4u);
    if (!bmp) eb_throw_oom(ctx);
    bmp->width = w;
    bmp->height = h;
    bmp->stride = (int32_t)((size_t)w * 4u);
    bmp->bgra = (uint8_t*)bmp + EB_PIXEL_OFFSET;
    return bmp;
}

fz_pixmap* eb_bitmap_pixmap(fz_context* ctx, eb_bitmap* bmp)
{
    return fz_new_pixmap_with_bbox_and_data(ctx, fz_device_bgr(ctx), (fz_irect){0, 0, bmp->width, bmp->height}, NULL, 1, bmp->bgra);
}

void eb_make_opaque(fz_pixmap* pix)
{
    for (size_t i = 3; i < (size_t)pix->stride * (size_t)pix->h; i += 4) pix->samples[i] = 255;
}

static void dims_of(fz_rect b, float scale, int32_t max_side, int* w, int* h, float* used)
{
    double bw = (double)b.x1 - (double)b.x0;
    double bh = (double)b.y1 - (double)b.y0;
    double s = max_side > 0 ? (double)max_side / fmax(fmax(bw, bh), 1.0) : (double)scale;
    double dw = ceil(bw * s);
    double dh = ceil(bh * s);
    if (!(dw >= 1.0)) dw = 1.0;
    if (!(dh >= 1.0)) dh = 1.0;
    if (max_side > 0) {
        dw = fmin(dw, (double)max_side);
        dh = fmin(dh, (double)max_side);
    }
    *used = (float)s;
    *w = dw <= EB_MAX_SIDE_PX ? (int)dw : -1;
    *h = dh <= EB_MAX_SIDE_PX ? (int)dh : -1;
}

static void draw(eb_doc* d, fz_page* pg, fz_rect b, float s, fz_pixmap* pix, const render_req* rq)
{
    fz_context* ctx = d->ctx;
    fz_device* dev = NULL;
    fz_matrix m = fz_concat(fz_translate(-b.x0, -b.y0), fz_scale(s, s));
    fz_clear_pixmap_with_value(ctx, pix, 255);
    fz_var(dev);
    fz_try(ctx)
    {
        dev = fz_new_draw_device(ctx, m, pix);
        fz_run_page(ctx, pg, dev, fz_identity, &d->cookie);
        fz_close_device(ctx, dev);
        eb_check_abort(d);
    }
    fz_always(ctx) fz_drop_device(ctx, dev);
    fz_catch(ctx) fz_rethrow(ctx);
    if (rq->tint) fz_tint_pixmap(ctx, pix, (int)rq->fg, (int)rq->bg);
    eb_make_opaque(pix);
}

static eb_status render_impl(eb_doc* d, int32_t page, const render_req* rq, eb_bitmap** out)
{
    fz_context* ctx = d->ctx;
    fz_page* pg = NULL;
    fz_pixmap* pix = NULL;
    eb_bitmap* bmp = NULL;
    eb_status st = eb_check_page(d, page);
    if (st != EB_OK) return st;
    eb_begin_call(d);
    fz_var(pg);
    fz_var(pix);
    fz_var(bmp);
    fz_try(ctx)
    {
        int w = 0, h = 0;
        float s = 1.0f;
        pg = fz_load_page(ctx, d->doc, page);
        fz_rect b = fz_bound_page(ctx, pg);
        dims_of(b, rq->scale, rq->max_side, &w, &h, &s);
        bmp = eb_new_bitmap(ctx, w, h);
        pix = eb_bitmap_pixmap(ctx, bmp);
        draw(d, pg, b, s, pix, rq);
    }
    fz_always(ctx)
    {
        fz_drop_pixmap(ctx, pix);
        fz_drop_page(ctx, pg);
    }
    fz_catch(ctx)
    {
        free(bmp);
        return eb_fail(ctx);
    }
    *out = bmp;
    return EB_OK;
}

eb_status eb_render_page(eb_doc* doc, int32_t page, const eb_render_opts* opts, eb_bitmap** out)
{
    render_req rq = {0};
    if (!doc || !opts || !out) return eb_fail_msg(EB_E_ARG, "null argument");
    *out = NULL;
    if (!isfinite(opts->scale) || opts->scale <= 0.0f) return eb_fail_msg(EB_E_ARG, "bad scale");
    rq.scale = opts->scale;
    rq.tint = opts->tint != 0;
    rq.fg = opts->fg_rgb;
    rq.bg = opts->bg_rgb;
    return render_impl(doc, page, &rq, out);
}

eb_status eb_render_cover(eb_doc* doc, int32_t max_side_px, eb_bitmap** out)
{
    render_req rq = {0};
    if (!doc || !out) return eb_fail_msg(EB_E_ARG, "null argument");
    *out = NULL;
    if (max_side_px < 1 || max_side_px > EB_MAX_COVER_SIDE) return eb_fail_msg(EB_E_ARG, "bad cover size");
    rq.max_side = max_side_px;
    if (doc->mobi.cover && eb_render_embedded_cover(doc, max_side_px, out) == EB_OK) return EB_OK;
    return render_impl(doc, 0, &rq, out);
}

void eb_bitmap_free(eb_bitmap* bmp) { free(bmp); }
