#include "Theme.h"

#include "Kv.h"

#include <array>
#include <format>

namespace eb {
namespace {

constexpr double kDarkLuma = 0.5;
constexpr std::size_t kHexLen = 7;
constexpr std::string_view kKeyTheme = "ui.theme", kKeyCustom = "ui.theme.custom", kKeyKeep = "ui.theme.keep_images";

struct IdName {
    ThemeId id{};
    std::string_view name;
};
constexpr std::array<IdName, 4> kIds{{{ThemeId::White, "white"}, {ThemeId::Black, "black"}, {ThemeId::Paper, "paper"},
    {ThemeId::Custom, "custom"}}};

int hex_digit(char c) noexcept
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

ThemeId id_from_name(std::string_view s) noexcept
{
    for (const auto& e : kIds) {
        if (e.name == s)
            return e.id;
    }
    return ThemeId::White;
}

std::string_view name_of(ThemeId id) noexcept
{
    for (const auto& e : kIds) {
        if (e.id == id)
            return e.name;
    }
    return "white";
}

}

ThemeColors preset_colors(ThemeId id) noexcept
{
    switch (id) {
    case ThemeId::Black:
        return {0x121212, 0xD0D0D0, 0x8AB4F8, 0x5C4B00};
    case ThemeId::Paper:
        return {0xF4ECD8, 0x3B2F1E, 0x7A4B12, 0xE8C872};
    default:
        return {};
    }
}

ThemeColors resolve_theme(const ThemePrefs& p) noexcept
{
    return p.id == ThemeId::Custom ? p.custom : preset_colors(p.id);
}

std::optional<uint32_t> parse_hex_rgb(std::string_view s) noexcept
{
    if (s.size() != kHexLen || s[0] != '#')
        return std::nullopt;
    uint32_t v = 0;
    for (std::size_t i = 1; i < kHexLen; ++i) {
        const int d = hex_digit(s[i]);
        if (d < 0)
            return std::nullopt;
        v = (v << 4) | static_cast<uint32_t>(d);
    }
    return v;
}

std::string format_hex_rgb(uint32_t rgb)
{
    return std::format("#{:06X}", rgb & 0xFFFFFF);
}

std::optional<ThemeColors> parse_custom_colors(std::string_view s)
{
    std::array<uint32_t, 4> v{};
    for (std::size_t i = 0; i < v.size(); ++i) {
        const std::size_t at = i * (kHexLen + 1);
        if (s.size() < at + kHexLen || (i > 0 && s[at - 1] != ' '))
            return std::nullopt;
        const auto c = parse_hex_rgb(s.substr(at, kHexLen));
        if (!c)
            return std::nullopt;
        v[i] = *c;
    }
    if (s.size() != 4 * kHexLen + 3)
        return std::nullopt;
    return ThemeColors{v[0], v[1], v[2], v[3]};
}

std::string format_custom_colors(const ThemeColors& c)
{
    return std::format("{} {} {} {}", format_hex_rgb(c.bg), format_hex_rgb(c.fg), format_hex_rgb(c.link),
        format_hex_rgb(c.highlight));
}

double luminance(uint32_t rgb) noexcept
{
    constexpr double k = 255.0;
    return (0.2126 * ((rgb >> 16) & 0xFF) + 0.7152 * ((rgb >> 8) & 0xFF) + 0.0722 * (rgb & 0xFF)) / k;
}

bool wants_dark_title(uint32_t bg_rgb) noexcept
{
    return luminance(bg_rgb) < kDarkLuma;
}

std::string theme_css()
{
    return "html, body { background-color: transparent !important; }";
}

bool wants_tint(const ThemePrefs& p, std::string_view fmt) noexcept
{
    return p.id != ThemeId::White && !(fmt == "cbz" && p.keep_images);
}

ThemePrefs load_theme_prefs(Db& db)
{
    ThemePrefs p;
    if (const auto v = kv_get(db, kKeyTheme))
        p.id = id_from_name(*v);
    if (const auto v = kv_get(db, kKeyCustom)) {
        if (const auto c = parse_custom_colors(*v))
            p.custom = *c;
    }
    if (const auto v = kv_get(db, kKeyKeep))
        p.keep_images = *v != "0";
    return p;
}

bool save_theme_prefs(Db& db, const ThemePrefs& p)
{
    const bool a = kv_set(db, kKeyTheme, name_of(p.id));
    const bool b = kv_set(db, kKeyCustom, format_custom_colors(p.custom));
    return kv_set(db, kKeyKeep, p.keep_images ? "1" : "0") && a && b;
}

}
