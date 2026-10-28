#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace eb {

class Db;

enum class ThemeId : uint8_t { White, Black, Paper, Custom };

struct ThemeColors {
    uint32_t bg = 0xFFFFFF, fg = 0x1A1A1A, link = 0x1A5FB4, highlight = 0xFFE58A; // 0xRRGGBB
    friend bool operator==(const ThemeColors&, const ThemeColors&) = default;
};

struct ThemePrefs {
    ThemeId id = ThemeId::White;
    ThemeColors custom;
    bool keep_images = true; // CBZ only: leave pictures untinted
    friend bool operator==(const ThemePrefs&, const ThemePrefs&) = default;
};

// What the worker needs to lay out and render one book with the current theme and font settings.
struct ReaderStyle {
    ThemeColors colors;
    bool tint = false;     // the engine maps black->fg and white->bg after rendering
    float em_pt = 12.0F;
    std::string css;       // user stylesheet for reflowable books
};

[[nodiscard]] ThemeColors preset_colors(ThemeId id) noexcept;
[[nodiscard]] ThemeColors resolve_theme(const ThemePrefs& p) noexcept;
// "#RRGGBB" only (either case); anything else is rejected.
[[nodiscard]] std::optional<uint32_t> parse_hex_rgb(std::string_view s) noexcept;
[[nodiscard]] std::string format_hex_rgb(uint32_t rgb);
[[nodiscard]] std::optional<ThemeColors> parse_custom_colors(std::string_view s);
[[nodiscard]] std::string format_custom_colors(const ThemeColors& c);
[[nodiscard]] double luminance(uint32_t rgb) noexcept;
[[nodiscard]] bool wants_dark_title(uint32_t bg_rgb) noexcept;
[[nodiscard]] std::string theme_css();
// Pages are tinted (black->fg, white->bg) for every theme but White, except CBZ pictures when the user keeps them.
[[nodiscard]] bool wants_tint(const ThemePrefs& p, std::string_view fmt) noexcept;

[[nodiscard]] ThemePrefs load_theme_prefs(Db& db);
bool save_theme_prefs(Db& db, const ThemePrefs& p);

}
