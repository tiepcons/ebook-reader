#include "ReaderView.h"

#include <algorithm>
#include <array>

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

void ReaderView::on_paint()
{
    PAINTSTRUCT ps;
    BeginPaint(hwnd_, &ps);
    const SIZE c = content_size(), v = client_size();
    const int ox = axis_origin(v.cx, c.cx, pan_x_), oy = axis_origin(v.cy, c.cy, pan_y_);
    const SpreadLayout lay = layout_now();
    const std::array<PageDraw, 2> pages{PageDraw{shown_.Get(), ox + lay.left_x, oy + lay.left_y},
        PageDraw{shown_right_.Get(), ox + lay.right_x, oy + lay.right_y}};
    canvas_.paint(pages, colors().bg, highlight_rects(ox, oy), highlight_color());
    EndPaint(hwnd_, &ps);
    if (canvas_.epoch() != epoch_) {
        check_epoch();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void ReaderView::set_shown(BitmapRef b, BitmapRef right)
{
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
