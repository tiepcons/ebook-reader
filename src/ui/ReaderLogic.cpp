#include "ReaderLogic.h"

#include "ReaderSpread.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <utility>

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

int32_t renumbered_page(int32_t left, int32_t shift, int32_t old_count, int32_t new_count, int pages) noexcept
{
    if (new_count <= 0)
        return 0;
    const bool on_last = old_count > 0 && left >= spread_start(old_count - 1, old_count, pages);
    const int32_t moved = on_last ? new_count - 1 : std::clamp(left + shift, 0, new_count - 1);
    return spread_start(moved, new_count, pages);
}

int32_t renumbered_focus(int32_t reading, int32_t shift, int32_t left, int32_t count, int pages) noexcept
{
    const int32_t now = std::clamp(reading + shift, 0, std::max(count - 1, 0));
    return now > left && now < left + std::max(pages, 1) ? now : -1;
}

void note_pending_text(std::vector<int32_t>& pending, int32_t page)
{
    if (std::ranges::find(pending, page) != pending.end())
        return;
    if (pending.size() >= RenderQueue::kMaxTextRequests)
        pending.erase(pending.begin());
    pending.push_back(page);
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

std::vector<TextBox> sentence_lines(const PageText& t, uint32_t begin, uint32_t end)
{
    std::vector<TextBox> out;
    const std::size_t n = std::min(t.offset.size(), t.box.size());
    auto i = static_cast<std::size_t>(std::lower_bound(t.offset.begin(), t.offset.begin() + static_cast<std::ptrdiff_t>(n), begin) - t.offset.begin());
    for (; i < n && t.offset[i] < end; ++i) {
        const TextBox& b = t.box[i];
        if (!(b.x1 > b.x0) || !(b.y1 > b.y0))
            continue;
        if (!out.empty()) {
            TextBox& cur = out.back();
            const float overlap = std::min(cur.y1, b.y1) - std::max(cur.y0, b.y0);
            if (overlap >= 0.5F * std::min(cur.y1 - cur.y0, b.y1 - b.y0)) {
                cur = {std::min(cur.x0, b.x0), std::min(cur.y0, b.y0), std::max(cur.x1, b.x1), std::max(cur.y1, b.y1)};
                continue;
            }
        }
        if (out.size() >= kMaxHighlightRects)
            break;
        out.push_back(b);
    }
    return out;
}

int toc_index_at(std::span<const TocItem> toc, uint64_t mark) noexcept
{
    int best = -1;
    for (std::size_t i = 0; i < toc.size(); ++i) {
        if (toc[i].target <= mark && (best < 0 || toc[i].target >= toc[static_cast<std::size_t>(best)].target))
            best = static_cast<int>(i);
    }
    return best;
}

int toc_prev_index(std::span<const TocItem> toc, uint64_t mark, int current) noexcept
{
    if (current >= 0)
        return current - 1;
    int best = -1;
    for (std::size_t i = 0; i < toc.size(); ++i) {
        if (toc[i].target < mark && (best < 0 || toc[i].target > toc[static_cast<std::size_t>(best)].target))
            best = static_cast<int>(i);
    }
    return best;
}

int toc_next_index(std::span<const TocItem> toc, uint64_t mark, int current) noexcept
{
    if (current < 0 && mark == kNoAnchor)
        return -1;
    const int next = (current >= 0 ? current : toc_index_at(toc, mark)) + 1;
    return static_cast<std::size_t>(next) < toc.size() ? next : -1;
}

const PageLink* link_at(std::span<const PageLink> links, float x, float y) noexcept
{
    const PageLink* best = nullptr;
    float best_area = 0;
    for (const PageLink& l : links) {
        const TextBox& a = l.area;
        if (!(x >= a.x0 && x < a.x1 && y >= a.y0 && y < a.y1))
            continue;
        const float area = (a.x1 - a.x0) * (a.y1 - a.y0);
        if (best == nullptr || area < best_area) {
            best = &l;
            best_area = area;
        }
    }
    return best;
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

void RenderQueue::post_text(int32_t page, uint32_t numbering)
{
    bool abort_step = false;
    {
        const std::lock_guard lock(mu_);
        const auto same = std::ranges::find(text_pages_, page, &std::pair<int32_t, uint32_t>::first);
        if (same != text_pages_.end()) {
            same->second = numbering;
        } else {
            if (text_pages_.size() >= kMaxTextRequests)
                text_pages_.pop_front();
            text_pages_.emplace_back(page, numbering);
        }
        abort_step = idle_running_;
    }
    cv_.notify_one();
    if (abort_step && abort_)
        abort_();
}

bool RenderQueue::pop(Job& out, const std::stop_token& st, std::chrono::milliseconds idle_after)
{
    std::unique_lock lock(mu_);
    const auto ready = [this] { return pending_.has_value() || !text_pages_.empty(); };
    if (idle_after.count() < 0) {
        if (!cv_.wait(lock, st, ready))
            return false;
    } else if (!cv_.wait_for(lock, st, idle_after, ready) && st.stop_requested()) {
        return false;
    }
    out.render = std::exchange(pending_, std::nullopt);
    out.text_page.reset();
    if (!text_pages_.empty()) {
        out.text_page = text_pages_.front().first;
        out.text_numbering = text_pages_.front().second;
        text_pages_.pop_front();
    }
    out.idle = !out.render && !out.text_page;
    idle_running_ = out.idle;
    in_flight_ = out.render.has_value() || out.idle;
    return true;
}

void RenderQueue::done() noexcept
{
    const std::lock_guard lock(mu_);
    in_flight_ = false;
    idle_running_ = false;
}

bool RenderQueue::current(uint64_t generation) const
{
    const std::lock_guard lock(mu_);
    return generation == gen_;
}

}
