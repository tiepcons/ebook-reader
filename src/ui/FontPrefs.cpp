#include "FontPrefs.h"

#include "Kv.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>

namespace eb {
namespace {

constexpr std::size_t kMaxFamilyBytes = 64;
constexpr std::string_view kKeyMode = "ui.font.mode", kKeyFamily = "ui.font.family", kKeySize = "ui.font.size_pt",
                           kKeyLine = "ui.font.line_height", kKeyMargin = "ui.font.margin_pt";
constexpr std::string_view kFamilySelector = "body,p,div,span,li,blockquote";
constexpr std::string_view kSizeSelector = "body,p,div,li,blockquote";

bool is_generic(std::string_view f) noexcept
{
    return f == "serif" || f == "sans-serif" || f == "monospace";
}

std::string family_css(std::string_view family)
{
    if (is_generic(family))
        return std::format("{}{{font-family:{} !important}}", kFamilySelector, family);
    return std::format("{}{{font-family:\"{}\",serif !important}}", kFamilySelector, family);
}

int read_int(Db& db, std::string_view key, int fallback) noexcept
{
    const auto v = kv_get(db, key);
    int out = fallback;
    if (v && std::from_chars(v->data(), v->data() + v->size(), out).ec != std::errc{})
        return fallback;
    return out;
}

FontMode mode_from(std::string_view s) noexcept
{
    return s == "family" ? FontMode::Family : (s == "all" ? FontMode::All : FontMode::Book);
}

std::string_view mode_name(FontMode m) noexcept
{
    return m == FontMode::Family ? "family" : (m == FontMode::All ? "all" : "book");
}

}

bool valid_family(std::string_view name) noexcept
{
    if (name.empty() || name.size() > kMaxFamilyBytes)
        return false;
    return std::ranges::all_of(name, [](char ch) {
        const auto c = static_cast<unsigned char>(ch);
        return c >= 0x80 || std::isalnum(c) != 0 || c == ' ' || c == '-' || c == '_' || c == '.' || c == '+';
    });
}

FontPrefs clamped(FontPrefs f)
{
    f.size_pt = std::clamp(f.size_pt, kMinSizePt, kMaxSizePt);
    f.line_height_pct = std::clamp(f.line_height_pct, kMinLinePct, kMaxLinePct);
    f.margin_pt = std::clamp(f.margin_pt, 0, kMaxMarginPt);
    if (!valid_family(f.family))
        f.family = "serif";
    return f;
}

std::string font_css(const FontPrefs& in)
{
    if (in.mode == FontMode::Book)
        return {};
    const FontPrefs f = clamped(in);
    std::string css = family_css(f.family);
    if (f.mode == FontMode::All) {
        css += std::format("{}{{font-size:{}pt !important;line-height:{} !important}}", kSizeSelector, f.size_pt,
            f.line_height_pct / 100.0);
        css += std::format("body{{margin:0 !important;padding:{}pt !important}}", f.margin_pt);
    }
    return css;
}

ReaderStyle make_style(const ReaderPrefs& p, std::string_view fmt)
{
    ReaderStyle s;
    s.colors = resolve_theme(p.theme);
    s.tint = wants_tint(p.theme, fmt);
    s.em_pt = static_cast<float>(clamped(p.font).size_pt);
    s.css = theme_css() + font_css(p.font);
    return s;
}

ReaderPrefs load_reader_prefs(Db& db)
{
    ReaderPrefs p;
    p.theme = load_theme_prefs(db);
    if (const auto v = kv_get(db, kKeyMode))
        p.font.mode = mode_from(*v);
    if (auto v = kv_get(db, kKeyFamily))
        p.font.family = std::move(*v);
    p.font.size_pt = read_int(db, kKeySize, kDefaultSizePt);
    p.font.line_height_pct = read_int(db, kKeyLine, kDefaultLinePct);
    p.font.margin_pt = read_int(db, kKeyMargin, 0);
    p.font = clamped(std::move(p.font));
    return p;
}

bool save_reader_prefs(Db& db, const ReaderPrefs& p)
{
    const FontPrefs f = clamped(p.font);
    bool ok = save_theme_prefs(db, p.theme);
    ok = kv_set(db, kKeyMode, mode_name(f.mode)) && ok;
    ok = kv_set(db, kKeyFamily, f.family) && ok;
    ok = kv_set(db, kKeySize, std::to_string(f.size_pt)) && ok;
    ok = kv_set(db, kKeyLine, std::to_string(f.line_height_pct)) && ok;
    return kv_set(db, kKeyMargin, std::to_string(f.margin_pt)) && ok;
}

}
