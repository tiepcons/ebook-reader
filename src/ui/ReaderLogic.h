#pragma once
#include "Theme.h"

#include <chrono>
#include <condition_variable>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eb {

inline constexpr float kReaderMarginDip = 24.0F;
inline constexpr float kBaseEmPt = 12.0F;
inline constexpr float kMinLayoutPt = 120.0F;
inline constexpr float kMinZoomPct = 50.0F;
inline constexpr float kMaxZoomPct = 400.0F;
inline constexpr float kZoomStep = 1.25F;
inline constexpr std::size_t kBitmapCacheBytes = std::size_t{48} << 20;
inline constexpr std::size_t kMaxBitmapBytes = std::size_t{32} << 20;
inline constexpr std::size_t kPrefetchMaxBytes = std::size_t{16} << 20;
inline constexpr uint32_t kDefaultMaxSidePx = 4096;
inline constexpr uint64_t kNoAnchor = UINT64_MAX;

struct LayoutParams {
    float width_pt = 0, height_pt = 0, em_pt = 0;
    friend bool operator==(const LayoutParams&, const LayoutParams&) = default;
};

// Device pixels per point: 1.0 point = 1/72 inch.
[[nodiscard]] float render_scale(unsigned dpi) noexcept;
// Content area (client minus margins) in points; DIP -> pt is x0.75.
[[nodiscard]] LayoutParams reflow_layout(int client_w_px, int client_h_px, unsigned dpi) noexcept;

struct ZoomState {
    enum class Fit : uint8_t { Page, Width, Custom };
    Fit fit = Fit::Page;
    float percent = 100.0F; // used by Custom; 100 % = actual size at the display DPI
    friend bool operator==(const ZoomState&, const ZoomState&) = default;
};

[[nodiscard]] float clamp_zoom(float pct) noexcept;
// Device pixels per point for a fixed-layout page; bounded so that one bitmap stays renderable.
[[nodiscard]] float fixed_scale(const ZoomState& z, float page_w_pt, float page_h_pt, int client_w, int client_h,
    unsigned dpi, uint32_t max_side_px) noexcept;
// Reduces a scale so the bitmap of a page stays within max_side_px per side and kMaxBitmapBytes in total.
[[nodiscard]] float limit_scale(float scale, float page_w_pt, float page_h_pt, uint32_t max_side_px) noexcept;
[[nodiscard]] float zoom_percent_of(float scale, unsigned dpi) noexcept;
[[nodiscard]] ZoomState zoom_step(float current_scale, unsigned dpi, int direction) noexcept;
[[nodiscard]] ZoomState toggle_fit_width(const ZoomState& z) noexcept;
// Pan offset limited to the part of the content that does not fit in the viewport.
[[nodiscard]] int clamp_pan(int pan, int content, int viewport) noexcept;
// Where the content starts inside the viewport: centred when it fits, otherwise shifted by the pan offset.
[[nodiscard]] int axis_origin(int viewport, int content, int pan) noexcept;
[[nodiscard]] std::size_t bitmap_bytes(int width, int height) noexcept;

enum class NavAction : uint8_t {
    None, Prev, Next, First, Last, ZoomIn, ZoomOut, ToggleFitWidth, Fullscreen, Close, Settings, Toc, Bookmark, GoTo
};

[[nodiscard]] NavAction nav_for_key(unsigned vk, bool shift, bool ctrl) noexcept;
// The outer quarters of the page turn it.
[[nodiscard]] NavAction nav_for_click(int x, int client_w) noexcept;

struct TocItem {
    int32_t level = 0; // 0 = top level
    uint64_t target = 0; // eb_bookmark
    std::string title;   // UTF-8
};

// Index of the outline entry that contains `mark`: the one with the largest target <= mark (the later one on a tie), -1 if none.
[[nodiscard]] int toc_index_at(std::span<const TocItem> toc, uint64_t mark) noexcept;

// Outline entry to open for the previous / next chapter button, -1 when there is none. Previous is the entry that starts
// closest before the shown page start `mark` (so inside a chapter it is that chapter's own start); next is the entry right
// after the current one. `current` is the entry the last jump chose while its page is still shown (several entries may
// start on one page), -1 to derive it from `mark`.
[[nodiscard]] int toc_prev_index(std::span<const TocItem> toc, uint64_t mark, int current) noexcept;
[[nodiscard]] int toc_next_index(std::span<const TocItem> toc, uint64_t mark, int current) noexcept;

// Position of a page in the book, 0..1; a one-page book counts as finished.
[[nodiscard]] double reading_fraction(int32_t page, int32_t count) noexcept;
// Page for a reading percentage (0..100) in a book of `count` pages; the inverse of page / (count - 1).
[[nodiscard]] int32_t page_for_percent(int percent, int32_t count) noexcept;
// The left page of the spread a view shows after the book was renumbered while its layout was still being completed: moved by
// `shift`, kept on the last spread when it was on it, always inside the book. `pages` is 1 or 2 (pages side by side).
[[nodiscard]] int32_t renumbered_page(int32_t left, int32_t shift, int32_t old_count, int32_t new_count, int pages) noexcept;
// The page inside the spread starting at `left` that read-aloud keeps following after the book was renumbered, -1 when it is the
// left page itself or no longer inside the spread. `reading` is the page followed before, in the old numbering.
[[nodiscard]] int32_t renumbered_focus(int32_t reading, int32_t shift, int32_t left, int32_t count, int pages) noexcept;
// Remembers a text request, forgetting the oldest ones past RenderQueue::kMaxTextRequests exactly as the queue does.
void note_pending_text(std::vector<int32_t>& pending, int32_t page);
// Whole number 0..100 with an optional trailing '%' and surrounding spaces.
[[nodiscard]] std::optional<int> parse_percent(std::wstring_view text) noexcept;
// Parent index of every outline entry (-1 for top level); jumps of more than one level attach to the nearest shallower entry.
[[nodiscard]] std::vector<int> toc_parents(std::span<const int32_t> levels);
// Indices of the bookmarks inside the page [start, end); `marks` need not be sorted.
[[nodiscard]] std::vector<std::size_t> marks_on_page(std::span<const uint64_t> marks, uint64_t start, uint64_t end);

struct TextBox {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0; // points, page space
};

// Text of one page with the box of each code point, as the reader needs it to read aloud and to highlight.
struct PageText {
    int32_t page = 0;
    std::string utf8;
    std::vector<uint32_t> offset; // byte offset of each code point
    std::vector<TextBox> box;
};

inline constexpr std::size_t kMaxPageChars = 200000;
inline constexpr std::size_t kMaxHighlightRects = 64;

// One rectangle per text line covering the code points whose offsets lie in [begin, end).
[[nodiscard]] std::vector<TextBox> sentence_lines(const PageText& t, uint32_t begin, uint32_t end);

// Internal link of a page: hot area in page points and the bookmark it leads to.
struct PageLink {
    TextBox area;
    uint64_t target = 0;
};

inline constexpr std::size_t kMaxPageLinks = 4096;

// The smallest link area containing the page point, nullptr when none does.
[[nodiscard]] const PageLink* link_at(std::span<const PageLink> links, float x, float y) noexcept;

struct ViewParams {
    int32_t client_w = 0, client_h = 0;
    uint32_t dpi = 96;
    int32_t pages = 1; // pages shown side by side: 1 or 2
    ZoomState zoom;
    friend bool operator==(const ViewParams&, const ViewParams&) = default;
};

struct RenderWant {
    uint64_t generation = 0; // assigned by RenderQueue::post
    uint32_t view_id = 0;    // UI epoch of the view parameters; results of another epoch are dropped by the UI
    ViewParams view;
    uint64_t anchor = kNoAnchor; // bookmark to jump to after layout
    uint64_t mark = kNoAnchor;   // confirmed bookmark of `page`, kNoAnchor while unknown
    uint32_t numbering = 0;      // the page numbering `page` is in (RenderResult::numbering of the last Layout result seen)
    uint32_t max_side_px = kDefaultMaxSidePx;
    int32_t page = 0;
    bool prefetch = true;
    // Bitmaps the UI already holds: the (left) page of the current / next / previous spread and, in a spread, its right page.
    bool have_cur = false, have_next = false, have_prev = false;
    bool have_cur2 = false, have_next2 = false, have_prev2 = false;
    std::shared_ptr<const ReaderStyle> style; // null = white theme, book fonts
};

// Latest-wins request slot: a newer request replaces an unserved one and aborts the one in flight.
class RenderQueue {
public:
    using AbortFn = std::function<void()>;

    // What the worker should do next: render, read the text of a page, or both; `idle` when there is neither, so the worker may
    // spend the time on the layout of the rest of the book (a request that arrives meanwhile aborts it).
    struct Job {
        std::optional<RenderWant> render;
        std::optional<int32_t> text_page;
        uint32_t text_numbering = 0;
        bool idle = false;
    };

    explicit RenderQueue(AbortFn abort) : abort_(std::move(abort)) {}

    uint64_t post(RenderWant w);
    // Text requests are served in order (duplicates merged, oldest dropped past kMaxTextRequests); they never cancel a
    // render in flight.
    void post_text(int32_t page, uint32_t numbering = 0);
    static constexpr size_t kMaxTextRequests = 4;
    // With idle_after >= 0 the call also returns, as an idle job, once that long has passed without any request.
    bool pop(Job& out, const std::stop_token& st, std::chrono::milliseconds idle_after = std::chrono::milliseconds(-1));
    // Makes every request so far stale and drops the pending one (used when closing).
    void supersede() noexcept;
    void done() noexcept;
    [[nodiscard]] bool current(uint64_t generation) const;

private:
    mutable std::mutex mu_;
    std::condition_variable_any cv_;
    std::optional<RenderWant> pending_;
    std::deque<std::pair<int32_t, uint32_t>> text_pages_; // page, numbering
    bool idle_running_ = false;
    uint64_t gen_ = 0;
    bool in_flight_ = false;
    AbortFn abort_;
};

// Least-recently-used values bounded by a byte budget; the most recent entry is always kept.
template <class V>
class ByteLru {
public:
    explicit ByteLru(std::size_t cap) noexcept : cap_(cap) {} // NOLINT(bugprone-exception-escape): an empty std::list allocates nothing the program could recover from

    void put(int32_t key, V value, std::size_t bytes)
    {
        erase(key);
        items_.push_front(Item{key, std::move(value), bytes});
        bytes_ += bytes;
        while (bytes_ > cap_ && items_.size() > 1) {
            bytes_ -= items_.back().bytes;
            items_.pop_back();
        }
    }
    V* get(int32_t key)
    {
        const auto it = find(key);
        if (it == items_.end())
            return nullptr;
        items_.splice(items_.begin(), items_, it);
        return std::addressof(items_.front().value);
    }
    // Drops every entry whose key lies outside [lo, hi].
    void keep_only(int32_t lo, int32_t hi)
    {
        for (auto it = items_.begin(); it != items_.end();) {
            if (it->key >= lo && it->key <= hi) {
                ++it;
                continue;
            }
            bytes_ -= it->bytes;
            it = items_.erase(it);
        }
    }
    void set_cap(std::size_t cap)
    {
        cap_ = cap;
        while (bytes_ > cap_ && items_.size() > 1) {
            bytes_ -= items_.back().bytes;
            items_.pop_back();
        }
    }
    [[nodiscard]] bool contains(int32_t key) const { return find(key) != items_.end(); }
    void clear() noexcept
    {
        items_.clear();
        bytes_ = 0;
    }
    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }

private:
    struct Item {
        int32_t key{};
        V value;
        std::size_t bytes{};
    };
    using Items = std::list<Item>;

    typename Items::iterator find(int32_t key)
    {
        auto it = items_.begin();
        while (it != items_.end() && it->key != key)
            ++it;
        return it;
    }
    typename Items::const_iterator find(int32_t key) const
    {
        auto it = items_.begin();
        while (it != items_.end() && it->key != key)
            ++it;
        return it;
    }
    void erase(int32_t key)
    {
        if (const auto it = find(key); it != items_.end()) {
            bytes_ -= it->bytes;
            items_.erase(it);
        }
    }

    Items items_;
    std::size_t cap_;
    std::size_t bytes_ = 0;
};

}
