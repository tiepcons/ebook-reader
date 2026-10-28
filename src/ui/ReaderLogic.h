#pragma once
#include "Theme.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
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
inline constexpr std::size_t kBitmapCacheBytes = std::size_t{96} << 20;
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

// Position of a page in the book, 0..1; a one-page book counts as finished.
[[nodiscard]] double reading_fraction(int32_t page, int32_t count) noexcept;
// Page for a reading percentage (0..100) in a book of `count` pages; the inverse of page / (count - 1).
[[nodiscard]] int32_t page_for_percent(int percent, int32_t count) noexcept;
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

struct ViewParams {
    int32_t client_w = 0, client_h = 0;
    uint32_t dpi = 96;
    ZoomState zoom;
    friend bool operator==(const ViewParams&, const ViewParams&) = default;
};

struct RenderWant {
    uint64_t generation = 0; // assigned by RenderQueue::post
    uint32_t view_id = 0;    // UI epoch of the view parameters; results of another epoch are dropped by the UI
    ViewParams view;
    uint64_t anchor = kNoAnchor; // bookmark to jump to after layout
    uint64_t mark = kNoAnchor;   // confirmed bookmark of `page`, kNoAnchor while unknown
    uint32_t max_side_px = kDefaultMaxSidePx;
    int32_t page = 0;
    bool prefetch = true;
    bool have_cur = false, have_next = false, have_prev = false; // bitmaps the UI already holds
    std::shared_ptr<const ReaderStyle> style; // null = white theme, book fonts
};

// Latest-wins request slot: a newer request replaces an unserved one and aborts the one in flight.
class RenderQueue {
public:
    using AbortFn = std::function<void()>;

    // What the worker should do next: render, read the text of a page, or both.
    struct Job {
        std::optional<RenderWant> render;
        std::optional<int32_t> text_page;
    };

    explicit RenderQueue(AbortFn abort) : abort_(std::move(abort)) {}

    uint64_t post(RenderWant w);
    // Latest-wins among text requests; it never cancels a render in flight.
    void post_text(int32_t page);
    bool pop(Job& out, std::stop_token st);
    // Makes every request so far stale and drops the pending one (used when closing).
    void supersede() noexcept;
    void done() noexcept;
    [[nodiscard]] bool current(uint64_t generation) const;

private:
    mutable std::mutex mu_;
    std::condition_variable_any cv_;
    std::optional<RenderWant> pending_;
    std::optional<int32_t> text_page_;
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
