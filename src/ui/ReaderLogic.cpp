#include "ReaderLogic.h"

#include <windows.h>

#include <algorithm>
#include <cmath>

namespace eb {
namespace {

constexpr float kPtPerDip = 0.75F;
constexpr float kMaxSidePx = 16384.0F;
constexpr int kEdgeDivisor = 4;

}

float render_scale(unsigned dpi) noexcept
{
    return static_cast<float>(dpi) / 72.0F;
}

LayoutParams reflow_layout(int client_w_px, int client_h_px, unsigned dpi) noexcept
{
    const float to_dip = 96.0F / static_cast<float>(std::max(dpi, 1U));
    const auto content_pt = [&](int px) {
        const float dip = static_cast<float>(std::max(px, 0)) * to_dip - 2.0F * kReaderMarginDip;
        return std::max(kMinLayoutPt, dip * kPtPerDip);
    };
    return {content_pt(client_w_px), content_pt(client_h_px), kBaseEmPt};
}

float clamp_zoom(float pct) noexcept
{
    if (!std::isfinite(pct))
        return 100.0F;
    return std::clamp(pct, kMinZoomPct, kMaxZoomPct);
}

float fixed_scale(const ZoomState& z, float page_w_pt, float page_h_pt, int client_w, int client_h, unsigned dpi,
    uint32_t max_side_px) noexcept
{
    const float actual = render_scale(dpi);
    if (!(page_w_pt > 0) || !(page_h_pt > 0) || client_w <= 0 || client_h <= 0)
        return actual;
    const auto cw = static_cast<float>(client_w), ch = static_cast<float>(client_h);
    float s = actual * clamp_zoom(z.percent) / 100.0F;
    if (z.fit == ZoomState::Fit::Page)
        s = std::min(cw / page_w_pt, ch / page_h_pt);
    else if (z.fit == ZoomState::Fit::Width)
        s = cw / page_w_pt;
    s = std::clamp(s, actual * kMinZoomPct / 100.0F, actual * kMaxZoomPct / 100.0F);
    return limit_scale(s, page_w_pt, page_h_pt, max_side_px);
}

float limit_scale(float scale, float page_w_pt, float page_h_pt, uint32_t max_side_px) noexcept
{
    if (!(page_w_pt > 0) || !(page_h_pt > 0))
        return scale;
    const float side = max_side_px > 1 ? static_cast<float>(max_side_px - 1) : kMaxSidePx;
    const float by_side = side / std::max(page_w_pt, page_h_pt);
    const float by_bytes = std::sqrt(static_cast<float>(kMaxBitmapBytes) / 4.0F / (page_w_pt * page_h_pt));
    return std::min({scale, by_side, by_bytes});
}

float zoom_percent_of(float scale, unsigned dpi) noexcept
{
    return scale / render_scale(dpi) * 100.0F;
}

ZoomState zoom_step(float current_scale, unsigned dpi, int direction) noexcept
{
    const float pct = clamp_zoom(zoom_percent_of(current_scale, dpi));
    return {ZoomState::Fit::Custom, clamp_zoom(direction > 0 ? pct * kZoomStep : pct / kZoomStep)};
}

ZoomState toggle_fit_width(const ZoomState& z) noexcept
{
    ZoomState out = z;
    out.fit = z.fit == ZoomState::Fit::Width ? ZoomState::Fit::Page : ZoomState::Fit::Width;
    return out;
}

int clamp_pan(int pan, int content, int viewport) noexcept
{
    return std::clamp(pan, 0, std::max(0, content - viewport));
}

int axis_origin(int viewport, int content, int pan) noexcept
{
    return content <= viewport ? (viewport - content) / 2 : -clamp_pan(pan, content, viewport);
}

std::size_t bitmap_bytes(int width, int height) noexcept
{
    return static_cast<std::size_t>(std::max(width, 0)) * static_cast<std::size_t>(std::max(height, 0)) * 4;
}

NavAction nav_for_key(unsigned vk, bool shift, bool ctrl) noexcept
{
    if (ctrl) {
        if (vk == VK_OEM_PLUS || vk == VK_ADD)
            return NavAction::ZoomIn;
        if (vk == VK_OEM_MINUS || vk == VK_SUBTRACT)
            return NavAction::ZoomOut;
        switch (vk) {
        case 'T':
            return NavAction::Toc;
        case 'B':
            return NavAction::Bookmark;
        case 'G':
            return NavAction::GoTo;
        case VK_OEM_COMMA:
            return NavAction::Settings;
        default:
            return NavAction::None;
        }
    }
    switch (vk) {
    case VK_LEFT:
    case VK_PRIOR:
        return NavAction::Prev;
    case VK_RIGHT:
    case VK_NEXT:
        return NavAction::Next;
    case VK_SPACE:
        return shift ? NavAction::Prev : NavAction::Next;
    case VK_HOME:
        return NavAction::First;
    case VK_END:
        return NavAction::Last;
    case VK_F11:
        return NavAction::Fullscreen;
    case VK_ESCAPE:
        return NavAction::Close;
    case 'W':
        return NavAction::ToggleFitWidth;
    default:
        return NavAction::None;
    }
}

double reading_fraction(int32_t page, int32_t count) noexcept
{
    return count <= 1 ? 1.0 : static_cast<double>(page) / (count - 1);
}

int32_t page_for_percent(int percent, int32_t count) noexcept
{
    if (count <= 1)
        return 0;
    const double f = std::clamp(percent, 0, 100) / 100.0;
    return static_cast<int32_t>(std::lround(f * (count - 1)));
}

std::optional<int> parse_percent(std::wstring_view text) noexcept
{
    while (!text.empty() && text.front() == L' ')
        text.remove_prefix(1);
    while (!text.empty() && text.back() == L' ')
        text.remove_suffix(1);
    if (!text.empty() && text.back() == L'%')
        text.remove_suffix(1);
    while (!text.empty() && text.back() == L' ')
        text.remove_suffix(1);
    if (text.empty() || text.size() > 3)
        return std::nullopt;
    int v = 0;
    for (const wchar_t c : text) {
        if (c < L'0' || c > L'9')
            return std::nullopt;
        v = v * 10 + (c - L'0');
    }
    return v <= 100 ? std::optional<int>(v) : std::nullopt;
}

std::vector<int> toc_parents(std::span<const int32_t> levels)
{
    std::vector<int> parents;
    parents.reserve(levels.size());
    std::vector<int> open; // indices of the entries on the path from the root to the previous entry
    for (std::size_t i = 0; i < levels.size(); ++i) {
        while (!open.empty() && levels[static_cast<std::size_t>(open.back())] >= levels[i])
            open.pop_back();
        parents.push_back(open.empty() ? -1 : open.back());
        open.push_back(static_cast<int>(i));
    }
    return parents;
}

std::vector<std::size_t> marks_on_page(std::span<const uint64_t> marks, uint64_t start, uint64_t end)
{
    std::vector<std::size_t> hits;
    for (std::size_t i = 0; i < marks.size(); ++i) {
        if (marks[i] >= start && marks[i] < end)
            hits.push_back(i);
    }
    return hits;
}

NavAction nav_for_click(int x, int client_w) noexcept
{
    if (client_w <= 0 || x < 0 || x >= client_w)
        return NavAction::None;
    if (x < client_w / kEdgeDivisor)
        return NavAction::Prev;
    return x >= client_w - client_w / kEdgeDivisor ? NavAction::Next : NavAction::None;
}

uint64_t RenderQueue::post(RenderWant w)
{
    bool abort_now = false;
    uint64_t gen = 0;
    {
        const std::lock_guard lock(mu_);
        gen = ++gen_;
        w.generation = gen;
        pending_ = w;
        abort_now = in_flight_;
    }
    cv_.notify_one();
    if (abort_now && abort_)
        abort_();
    return gen;
}

void RenderQueue::supersede() noexcept
{
    const std::lock_guard lock(mu_);
    ++gen_;
    pending_.reset();
}

bool RenderQueue::pop(RenderWant& out, std::stop_token st)
{
    std::unique_lock lock(mu_);
    if (!cv_.wait(lock, std::move(st),[this] { return pending_.has_value(); }))
        return false;
    out = *pending_;
    pending_.reset();
    in_flight_ = true;
    return true;
}

void RenderQueue::done() noexcept
{
    const std::lock_guard lock(mu_);
    in_flight_ = false;
}

bool RenderQueue::current(uint64_t generation) const
{
    const std::lock_guard lock(mu_);
    return generation == gen_;
}

}
