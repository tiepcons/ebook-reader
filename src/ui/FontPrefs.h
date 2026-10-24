#pragma once
#include "Theme.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace eb {

class Db;

enum class FontMode : uint8_t { Book, Family, All };

inline constexpr int kMinSizePt = 8, kMaxSizePt = 32, kDefaultSizePt = 12;
inline constexpr int kMinLinePct = 120, kMaxLinePct = 200, kDefaultLinePct = 150;
inline constexpr int kMaxMarginPt = 48;

struct FontPrefs {
    FontMode mode = FontMode::Book;
    std::string family = "serif";
    int size_pt = kDefaultSizePt;
    int line_height_pct = kDefaultLinePct;
    int margin_pt = 0;
    friend bool operator==(const FontPrefs&, const FontPrefs&) = default;
};

struct ReaderPrefs {
    ThemePrefs theme;
    FontPrefs font;
    friend bool operator==(const ReaderPrefs&, const ReaderPrefs&) = default;
};

// Brings every field into its allowed range; an unusable family name falls back to "serif".
[[nodiscard]] FontPrefs clamped(FontPrefs f);
// A family name that is safe to place inside a CSS string: no quotes, braces, backslashes or control characters.
[[nodiscard]] bool valid_family(std::string_view name) noexcept;
[[nodiscard]] std::string font_css(const FontPrefs& f);
[[nodiscard]] ReaderStyle make_style(const ReaderPrefs& p, std::string_view fmt);

[[nodiscard]] ReaderPrefs load_reader_prefs(Db& db);
bool save_reader_prefs(Db& db, const ReaderPrefs& p);

}
