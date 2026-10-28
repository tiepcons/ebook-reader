#pragma once
#include "Tray.h"

#include <windows.h>

#include <array>
#include <string>

namespace eb {

inline constexpr std::array<double, 6> kTtsSpeeds{0.75, 1.0, 1.25, 1.5, 1.75, 2.0};
inline constexpr std::array<const wchar_t*, 6> kTtsSpeedLabels{L"0,75×", L"1×", L"1,25×", L"1,5×", L"1,75×", L"2×"};

// Index of the speed closest to `rate`.
[[nodiscard]] int nearest_speed_index(double rate) noexcept;

// Slim strip of child controls under the reader: previous / play-pause / stop / next sentence, speed, voice settings
// and one line of status. Clicks arrive at the parent as WM_COMMAND with the kIdTts* ids.
class TtsBar {
public:
    bool create(HINSTANCE inst, HWND parent) noexcept;
    // Places the controls in the strip (x, y, w, h in pixels); `gap` is the spacing and `dpi` scales the fixed widths.
    void layout(int x, int y, int w, int h, int gap, UINT dpi) noexcept;
    void show(int cmd) noexcept;
    void set_font(HFONT font) noexcept;
    void set_state(TtsState s) noexcept;
    void set_info(const std::wstring& text) noexcept;
    // "Azure 75%"; empty hides the label.
    void set_quota(const std::wstring& text) noexcept;
    void set_speed(double rate) noexcept;
    [[nodiscard]] double speed() const noexcept;

private:
    HWND play_ = nullptr, stop_ = nullptr, prev_ = nullptr, next_ = nullptr, speed_ = nullptr, settings_ = nullptr,
         info_ = nullptr, quota_ = nullptr;
};

}
