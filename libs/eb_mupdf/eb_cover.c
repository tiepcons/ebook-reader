#include "eb_internal.h"

#include <math.h>

#define EB_MAX_SIDE_PX 32767
#define EB_COVER_OFFSET (((sizeof(eb_bitmap) + 15) / 16) * 16)

static void cover_dims(const fz_image* img, int32_t max_side, int* w, int* h)
{
    const double longer = (double)(img->w > img->h ? img->w : img->h);
    const double s = (double)max_side / (longer >= 1.0 ? longer : 1.0);
    double dw = ceil((double)img->w * s);
    double dh = ceil((double)img->h * s);
    if (dw < 1.0) dw = 1.0;
    if (dh < 1.0) dh = 1.0;
    *w = (int)fmin(dw, (double)max_side);
    *h = (int)fmin(dh, (double)max_side);
}

static void draw_cover(fz_context* ctx, fz_image* img, fz_pixmap* pix, int w, int h)
{
    fz_device* dev = NULL;
    fz_matrix m = {(float)w, 0, 0, -(float)h, 0, (float)h};
    fz_clear_pixmap_with_value(ctx, pix, 255);
    fz_var(dev);
    fz_try(ctx)
    {
        dev = fz_new_draw_device(ctx, fz_identity, pix);
        fz_fill_image(ctx, dev, img, m, 1.0f, fz_default_color_params);
        fz_close_device(ctx, dev);
    }
    fz_always(ctx) fz_drop_device(ctx, dev);
    fz_catch(ctx) fz_rethrow(ctx);
    for (size_t i = 3; i < (size_t)pix->stride * (size_t)pix->h; i += 4) pix->samples[i] = 255;
}

eb_status eb_render_embedded_cover(eb_doc* doc, int32_t max_side, eb_bitmap** out)
{
    fz_context* ctx = doc->ctx;
    fz_buffer* buf = NULL;
    fz_image* img = NULL;
    fz_pixmap* pix = NULL;
    eb_bitmap* bmp = NULL;
    fz_var(buf);
    fz_var(img);
    fz_var(pix);
    fz_var(bmp);
    fz_try(ctx)
    {
        int w = 0, h = 0;
        buf = fz_new_buffer_from_copied_data(ctx, doc->mobi.cover, doc->mobi.cover_size);
        img = fz_new_image_from_buffer(ctx, buf);
        if (img->w <= 0 || img->h <= 0) fz_throw(ctx, FZ_ERROR_FORMAT, "empty cover image");
        cover_dims(img, max_side, &w, &h);
        if (w > EB_MAX_SIDE_PX || h > EB_MAX_SIDE_PX || (double)w * (double)h > (double)eb_g_limits.max_bitmap_px)
            fz_throw(ctx, FZ_ERROR_LIMIT, "cover exceeds max_bitmap_px");
        bmp = malloc(EB_COVER_OFFSET + (size_t)w * (size_t)h * 4u);
        if (!bmp) eb_throw_oom(ctx);
        bmp->width = w;
        bmp->height = h;
        bmp->stride = (int32_t)((size_t)w * 4u);
        bmp->bgra = (uint8_t*)bmp + EB_COVER_OFFSET;
        pix = fz_new_pixmap_with_bbox_and_data(ctx, fz_device_bgr(ctx), (fz_irect){0, 0, w, h}, NULL, 1, bmp->bgra);
        draw_cover(ctx, img, pix, w, h);
    }
    fz_always(ctx)
    {
        fz_drop_pixmap(ctx, pix);
        fz_drop_image(ctx, img);
        fz_drop_buffer(ctx, buf);
    }
    fz_catch(ctx)
    {
        free(bmp);
        return eb_fail(ctx);
    }
    *out = bmp;
    return EB_OK;
}
