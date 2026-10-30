#include "ReaderSpread.h"

#include <algorithm>
#include <cmath>

namespace eb {
namespace {

float to_dip(int px, unsigned dpi) noexcept
{
    return static_cast<float>(std::max(px, 0)) * 96.0F / static_cast<float>(std::max(dpi, 1U));
}

}

bool wants_spread(int client_w_px, int client_h_px, unsigned dpi) noexcept
{
    if (client_w_px <= 0 || client_h_px <= 0)
        return false;
    const float w = to_dip(client_w_px, dpi), h = to_dip(client_h_px, dpi);
    return w >= 2.0F * kMinPageDip + kSpreadGutterDip && w / h >= kSpreadMinAspect;
}

int spread_gutter_px(unsigned dpi) noexcept
{
    return static_cast<int>(std::lround(kSpreadGutterDip * static_cast<float>(dpi) / 96.0F));
}

int slot_width_px(int client_w_px, unsigned dpi, int pages) noexcept
{
    if (pages < 2)
        return client_w_px;
    return std::max(0, (client_w_px - spread_gutter_px(dpi)) / 2);
}

int32_t spread_start(int32_t page, int32_t count, int pages) noexcept
{
    const int32_t p = std::clamp(page, 0, std::max(count - 1, 0));
    return pages >= 2 ? p - p % 2 : p;
}

int32_t spread_move(int32_t left, int32_t count, int pages, int steps) noexcept
{
    const int64_t span = pages >= 2 ? 2 : 1;
    const int64_t target = static_cast<int64_t>(left) + span * steps;
    return spread_start(static_cast<int32_t>(std::clamp<int64_t>(target, 0, std::max(count - 1, 0))), count, pages);
}

bool has_right_page(int32_t left, int32_t count, int pages) noexcept
{
    return pages >= 2 && left >= 0 && left + 1 < count;
}

SpreadLayout spread_layout(int left_w, int left_h, int right_w, int right_h, int gutter, bool spread) noexcept
{
    SpreadLayout out;
    if (!spread) {
        out.content_w = left_w;
        out.content_h = left_h;
        return out;
    }
    const int rw = right_w > 0 ? right_w : left_w, rh = right_h > 0 ? right_h : left_h;
    out.content_w = left_w + gutter + rw;
    out.content_h = std::max(left_h, rh);
    out.left_y = (out.content_h - left_h) / 2;
    out.right_x = left_w + gutter;
    out.right_y = (out.content_h - rh) / 2;
    return out;
}

WindowSize default_window_size(int work_w, int work_h) noexcept
{
    const int h = std::max(1, static_cast<int>(std::lround(static_cast<float>(work_h) * kDefaultHeightFraction)));
    const int w = std::max(1, static_cast<int>(std::lround(static_cast<float>(h) * kLetterAspect)));
    return {std::min(w, std::max(work_w, 1)), h};
}

}
