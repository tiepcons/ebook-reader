#pragma once
#include <cstdint>

namespace eb {

inline constexpr float kMinPageDip = 420.0F;
inline constexpr float kSpreadGutterDip = 8.0F;
inline constexpr float kSpreadMinAspect = 1.15F;
inline constexpr float kLetterAspect = 8.5F / 11.0F;
inline constexpr float kDefaultHeightFraction = 0.85F;

// Two pages side by side like an open book when the area is wide enough for both and not much taller than wide.
[[nodiscard]] bool wants_spread(int client_w_px, int client_h_px, unsigned dpi) noexcept;
[[nodiscard]] int spread_gutter_px(unsigned dpi) noexcept;
// Width available to one page: the whole client for a single page, half of it minus the gutter for a spread.
[[nodiscard]] int slot_width_px(int client_w_px, unsigned dpi, int pages) noexcept;

// Left page of the spread holding `page` (spreads are 0-based pairs 2k, 2k+1); `page` is clamped into the book.
[[nodiscard]] int32_t spread_start(int32_t page, int32_t count, int pages) noexcept;
// Left page after moving `steps` spreads from the one starting at `left`; stays inside the book.
[[nodiscard]] int32_t spread_move(int32_t left, int32_t count, int pages, int steps) noexcept;
// Whether the right slot of the spread starting at `left` holds a page (it is empty after the last page).
[[nodiscard]] bool has_right_page(int32_t left, int32_t count, int pages) noexcept;

// Placement of the pages inside the content rectangle; the right page is absent when `right_w` is 0, and an absent right
// slot (loading, or past the last page) takes the size of the left page.
struct SpreadLayout {
    int content_w = 0, content_h = 0;
    int left_x = 0, left_y = 0, right_x = 0, right_y = 0;
    friend bool operator==(const SpreadLayout&, const SpreadLayout&) = default;
};

[[nodiscard]] SpreadLayout spread_layout(int left_w, int left_h, int right_w, int right_h, int gutter, bool spread) noexcept;

struct WindowSize {
    int w = 0, h = 0;
};

// Outer size of the main window on first launch: portrait letter proportions, most of the work area's height.
[[nodiscard]] WindowSize default_window_size(int work_w, int work_h) noexcept;

}
