#include <doctest/doctest.h>

#include "lib/Db.h"
#include "lib/Schema.h"
#include "test_base_tmp.h"
#include "ui/FontPrefs.h"
#include "ui/Kv.h"
#include "ui/ReaderLogic.h"
#include "ui/Theme.h"

namespace {

eb::Db fresh(const ebtest::TempDir& t)
{
    auto db = eb::Db::open(t.path() / L"lib.db");
    REQUIRE(db.has_value());
    REQUIRE(eb::migrate(*db).has_value());
    return std::move(*db);
}

}

TEST_CASE("theme: presets")
{
    using eb::ThemeId;
    CHECK(eb::preset_colors(ThemeId::White) == eb::ThemeColors{0xFFFFFF, 0x1A1A1A, 0x1A5FB4, 0xFFE58A});
    CHECK(eb::preset_colors(ThemeId::Black) == eb::ThemeColors{0x121212, 0xD0D0D0, 0x8AB4F8, 0x5C4B00});
    CHECK(eb::preset_colors(ThemeId::Paper) == eb::ThemeColors{0xF4ECD8, 0x3B2F1E, 0x7A4B12, 0xE8C872});
    eb::ThemePrefs p;
    p.id = ThemeId::Custom;
    p.custom = {0x010203, 0x040506, 0x070809, 0x0A0B0C};
    CHECK(eb::resolve_theme(p) == p.custom);
}

TEST_CASE("theme: hex colour parsing rejects garbage")
{
    CHECK(eb::parse_hex_rgb("#1A5FB4") == 0x1A5FB4U);
    CHECK(eb::parse_hex_rgb("#1a5fb4") == 0x1A5FB4U);
    for (const char* bad : {"", "#", "1A5FB4", "#1A5FB", "#1A5FB40", "#1A5FBG", "#-1A5FB", " #1A5FB4", "#1A5FB4 ", "0x1A5FB4"})
        CHECK_FALSE(eb::parse_hex_rgb(bad).has_value());
    CHECK(eb::format_hex_rgb(0x0000FF) == "#0000FF");
    const eb::ThemeColors c{0x121212, 0xD0D0D0, 0x8AB4F8, 0x5C4B00};
    CHECK(eb::parse_custom_colors(eb::format_custom_colors(c)) == c);
    for (const char* bad : {"", "#121212", "#121212 #D0D0D0 #8AB4F8", "#121212 #D0D0D0 #8AB4F8 #5C4B0G",
             "#121212#D0D0D0 #8AB4F8 #5C4B00", "#121212 #D0D0D0 #8AB4F8 #5C4B00 #FFFFFF", "#121212  #D0D0D0 #8AB4F8 #5C4B00"})
        CHECK_FALSE(eb::parse_custom_colors(bad).has_value());
}

TEST_CASE("theme: luminance decides the title bar")
{
    CHECK(eb::luminance(0x000000) == doctest::Approx(0.0));
    CHECK(eb::luminance(0xFFFFFF) == doctest::Approx(1.0));
    CHECK(eb::wants_dark_title(0x121212));
    CHECK_FALSE(eb::wants_dark_title(0xFFFFFF));
    CHECK_FALSE(eb::wants_dark_title(0xF4ECD8));
    CHECK(eb::wants_dark_title(0x1F3A5F));
    CHECK_FALSE(eb::wants_dark_title(0x808080));
}

TEST_CASE("theme: css and tint decisions")
{
    CHECK(eb::theme_css() == "html, body { background-color: transparent !important; }");
    eb::ThemePrefs p;
    CHECK_FALSE(eb::wants_tint(p, "pdf"));
    CHECK_FALSE(eb::wants_tint(p, "cbz"));
    p.id = eb::ThemeId::Black;
    CHECK(eb::wants_tint(p, "pdf"));
    CHECK_FALSE(eb::wants_tint(p, "cbz"));
    p.keep_images = false;
    CHECK(eb::wants_tint(p, "cbz"));
    p.keep_images = true;
    CHECK(eb::wants_tint(p, "pdf"));
}

TEST_CASE("fonts: css per mode")
{
    eb::FontPrefs f;
    f.family = "Literata";
    CHECK(eb::font_css(f).empty());
    f.mode = eb::FontMode::Family;
    CHECK(eb::font_css(f) == "body,p,div,span,li,blockquote{font-family:\"Literata\",serif !important}");
    f.family = "sans-serif";
    CHECK(eb::font_css(f) == "body,p,div,span,li,blockquote{font-family:sans-serif !important}");
    f.mode = eb::FontMode::All;
    f.family = "Be Vietnam Pro";
    f.size_pt = 14;
    f.line_height_pct = 160;
    f.margin_pt = 20;
    CHECK(eb::font_css(f)
        == "body,p,div,span,li,blockquote{font-family:\"Be Vietnam Pro\",serif !important}"
           "body,p,div,li,blockquote{font-size:14pt !important;line-height:1.6 !important}"
           "body{margin:0 !important;padding:20pt !important}");
}

TEST_CASE("fonts: clamps and family validation")
{
    eb::FontPrefs f;
    f.size_pt = 2;
    f.line_height_pct = 500;
    f.margin_pt = -4;
    f = eb::clamped(f);
    CHECK(f.size_pt == eb::kMinSizePt);
    CHECK(f.line_height_pct == eb::kMaxLinePct);
    CHECK(f.margin_pt == 0);
    f.size_pt = 99;
    f.margin_pt = 99;
    f.line_height_pct = 1;
    f = eb::clamped(f);
    CHECK(f.size_pt == eb::kMaxSizePt);
    CHECK(f.margin_pt == eb::kMaxMarginPt);
    CHECK(f.line_height_pct == eb::kMinLinePct);
    CHECK(eb::valid_family("Be Vietnam Pro"));
    CHECK(eb::valid_family("Noto Sans"));
    for (const char* bad : {"", "a\"b", "a}b{", "a;b", "a\\b", "a\nb", "a/*b"})
        CHECK_FALSE(eb::valid_family(bad));
    f.family = "x\"}body{display:none";
    CHECK(eb::clamped(f).family == "serif");
}

TEST_CASE("fonts: style combines theme and font css")
{
    eb::ReaderPrefs p;
    p.theme.id = eb::ThemeId::Paper;
    p.font.size_pt = 16;
    const eb::ReaderStyle s = eb::make_style(p, "epub");
    CHECK(s.css == eb::theme_css());
    CHECK(s.em_pt == doctest::Approx(16.0));
    CHECK(s.colors.bg == 0xF4ECD8U);
    CHECK(s.tint);
}

TEST_CASE("theme: settings round-trip through kv and bad values fall back")
{
    const ebtest::TempDir t;
    auto db = fresh(t);
    CHECK(eb::load_reader_prefs(db) == eb::ReaderPrefs{});
    eb::ReaderPrefs p;
    p.theme.id = eb::ThemeId::Custom;
    p.theme.custom = {0x102030, 0x405060, 0x708090, 0xA0B0C0};
    p.theme.keep_images = false;
    p.font = {eb::FontMode::All, "Literata", 18, 170, 24};
    CHECK(eb::save_reader_prefs(db, p));
    CHECK(eb::load_reader_prefs(db) == p);
    CHECK(eb::kv_get(db, "ui.theme") == "custom");
    CHECK(eb::kv_get(db, "ui.theme.custom") == "#102030 #405060 #708090 #A0B0C0");
    CHECK(eb::kv_get(db, "ui.font.mode") == "all");
    CHECK(eb::kv_set(db, "ui.theme", "neon"));
    CHECK(eb::kv_set(db, "ui.theme.custom", "garbage"));
    CHECK(eb::kv_set(db, "ui.font.size_pt", "999"));
    CHECK(eb::kv_set(db, "ui.font.line_height", "x"));
    CHECK(eb::kv_set(db, "ui.font.family", "a\"b"));
    const eb::ReaderPrefs q = eb::load_reader_prefs(db);
    CHECK(q.theme.id == eb::ThemeId::White);
    CHECK(q.theme.custom == eb::ThemeColors{});
    CHECK(q.font.size_pt == eb::kMaxSizePt);
    CHECK(q.font.line_height_pct == eb::kDefaultLinePct);
    CHECK(q.font.family == "serif");
}

TEST_CASE("reader: Ctrl+T opens the settings")
{
    CHECK(eb::nav_for_key('T', false, true) == eb::NavAction::Settings);
    CHECK(eb::nav_for_key('T', false, false) == eb::NavAction::None);
}
