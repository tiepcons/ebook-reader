#include "eb_internal.h"

#include <math.h>

static void cover_dims(const fz_image* img, int swap, int32_t max_side, int* w, int* h)
{
    const double iw = swap ? img->h : img->w;
    const double ih = swap ? img->w : img->h;
    const double s = (double)max_side / (iw > ih ? iw : ih);
    *w = (int)fmin(fmax(ceil(iw * s), 1.0), (double)max_side);
    *h = (int)fmin(fmax(ceil(ih * s), 1.0), (double)max_side);
}

/* Same placement as MuPDF's image documents: orientation matrix first, then scale to the target size. */
static void draw_cover(fz_context* ctx, fz_image* img, fz_pixmap* pix, int w, int h)
{
    fz_device* dev = NULL;
    const fz_matrix m = fz_post_scale(fz_image_orientation_matrix(ctx, img), (float)w, (float)h);
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
    eb_make_opaque(pix);
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
        buf = doc->cover_buf ? fz_keep_buffer(ctx, doc->cover_buf) : fz_new_buffer_from_copied_data(ctx, doc->mobi.cover, doc->mobi.cover_size);
        img = fz_new_image_from_buffer(ctx, buf);
        if (img->w <= 0 || img->h <= 0) fz_throw(ctx, FZ_ERROR_FORMAT, "empty cover image");
        const uint8_t orientation = fz_image_orientation(ctx, img);
        if ((int64_t)img->w * (int64_t)img->h > (int64_t)eb_g_limits.max_bitmap_px)
            fz_throw(ctx, FZ_ERROR_LIMIT, "cover image exceeds max_bitmap_px");
        cover_dims(img, orientation != 0 && (orientation & 1) == 0, max_side, &w, &h);
        bmp = eb_new_bitmap(ctx, w, h);
        pix = eb_bitmap_pixmap(ctx, bmp);
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
