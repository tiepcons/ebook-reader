#include "ReaderView.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace eb {
namespace {

SIZE pixel_size(const BitmapRef& b) noexcept
{
    if (!b)
        return {0, 0};
    const D2D1_SIZE_U sz = b->GetPixelSize();
    return {static_cast<LONG>(sz.width), static_cast<LONG>(sz.height)};
}

}

SpreadLayout ReaderView::layout_now() const noexcept
{
    const SIZE l = pixel_size(shown_), r = pixel_size(shown_right_);
    return spread_layout(l.cx, l.cy, r.cx, r.cy, spread_gutter_px(view_.dpi), spread());
}

SIZE ReaderView::content_size() const noexcept
{
    if (!shown_)
        return {0, 0};
    const SpreadLayout lay = layout_now();
    return {lay.content_w, lay.content_h};
}

// While the window is dragged to a new size the pages are not laid out again: the ones shown are scaled to fit.
float ReaderView::preview_scale(SIZE content, SIZE client) const noexcept
{
    const bool resized = client.cx != view_.client_w || client.cy != view_.client_h;
    if (!live_resize_ || !resized || content.cx <= 0 || content.cy <= 0 || client.cx <= 0 || client.cy <= 0)
        return 1.0F;
    return std::min(static_cast<float>(client.cx) / static_cast<float>(content.cx),
        static_cast<float>(client.cy) / static_cast<float>(content.cy));
}

void ReaderView::on_paint()
{
    PAINTSTRUCT ps;
    BeginPaint(hwnd_, &ps);
    const SIZE c = content_size(), v = client_size();
    const float k = preview_scale(c, v);
    const auto scaled = [k](int n) { return static_cast<int>(std::lround(static_cast<float>(n) * k)); };
    const int ox = k == 1.0F ? axis_origin(v.cx, c.cx, pan_x_) : (v.cx - scaled(c.cx)) / 2;
    const int oy = k == 1.0F ? axis_origin(v.cy, c.cy, pan_y_) : (v.cy - scaled(c.cy)) / 2;
    const SpreadLayout lay = layout_now();
    const std::array<PageDraw, 2> pages{PageDraw{shown_.Get(), ox + scaled(lay.left_x), oy + scaled(lay.left_y), k},
        PageDraw{shown_right_.Get(), ox + scaled(lay.right_x), oy + scaled(lay.right_y), k}};
    canvas_.paint(pages, colors().bg, k == 1.0F ? highlight_rects(ox, oy) : std::vector<D2D1_RECT_F>{}, highlight_color());
    EndPaint(hwnd_, &ps);
    if (canvas_.epoch() != epoch_) {
        check_epoch();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void ReaderView::set_shown(BitmapRef b, BitmapRef right)
{
    shown_page_ = b ? page_ : -1;
    shown_ = std::move(b);
    shown_right_ = std::move(right);
    const SIZE l = pixel_size(shown_), r = pixel_size(shown_right_);
    const std::size_t held = bitmap_bytes(l.cx, l.cy) + bitmap_bytes(r.cx, r.cy);
    cache_.set_cap(kBitmapCacheBytes - std::min(kBitmapCacheBytes, held));
}

void ReaderView::show_cached()
{
    BitmapRef* l = cache_.get(page_);
    if (l == nullptr)
        return;
    BitmapRef left = *l;
    BitmapRef right;
    if (has_right_page(page_, count_, view_.pages)) {
        if (BitmapRef* r = cache_.get(page_ + 1))
            right = *r;
    }
    set_shown(std::move(left), std::move(right));
}

void ReaderView::rekey_cache(int32_t shift)
{
    if (shift == 0) {
        cache_.keep_only(page_ - span(), page_ + 2 * span() - 1);
        return;
    }
    const BitmapRef left = shown_, right = shown_right_;
    cache_.clear();
    if (shown_page_ < 0)
        return;
    shown_page_ = std::max(shown_page_ + shift, 0);
    const SIZE l = pixel_size(left), r = pixel_size(right);
    if (left)
        cache_.put(shown_page_, left, bitmap_bytes(l.cx, l.cy));
    if (right)
        cache_.put(shown_page_ + 1, right, bitmap_bytes(r.cx, r.cy));
}

bool ReaderView::cached_page(int32_t page) const
{
    return page >= 0 && page < count_ && cache_.contains(page);
}

bool ReaderView::cached_second(int32_t first) const
{
    return first >= 0 && first < count_ && (!has_right_page(first, count_, view_.pages) || cache_.contains(first + 1));
}

int32_t ReaderView::reading_page() const noexcept
{
    const bool inside = focus_page_ >= page_ && focus_page_ < page_ + span();
    return inside ? focus_page_ : page_;
}

void ReaderView::turn_to(int32_t page)
{
    if (count_ <= 0)
        return;
    page = std::clamp(page, 0, count_ - 1);
    if (page < page_ || page >= page_ + span())
        go_to(page);
    focus_page_ = page;
}

}
