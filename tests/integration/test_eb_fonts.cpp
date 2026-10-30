#include "eb_test_util.h"
#include "../unit/test_perf.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

namespace {

fs::path system_fonts()
{
    wchar_t win[MAX_PATH] = {};
    GetSystemWindowsDirectoryW(win, MAX_PATH);
    return fs::path(win) / "Fonts";
}

struct TempDir {
    fs::path path;
    TempDir()
    {
        static int counter = 0;
        path = fs::temp_directory_path() / ("eb_font_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                                            "_" + std::to_string(++counter));
        fs::create_directories(path);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

eb_status set_dirs(const std::vector<fs::path>& dirs)
{
    std::vector<std::wstring> keep;
    std::vector<const wchar_t*> ptrs;
    keep.reserve(dirs.size());
    for (const auto& d : dirs) keep.push_back(d.wstring());
    ptrs.reserve(keep.size());
    for (const auto& s : keep) ptrs.push_back(s.c_str());
    return eb_set_font_dirs(ptrs.data(), static_cast<int32_t>(ptrs.size()));
}

std::vector<std::string> families()
{
    char** names = nullptr;
    int32_t n = -1;
    REQUIRE(eb_font_families(&names, &n) == EB_OK);
    std::vector<std::string> out;
    out.reserve(static_cast<size_t>(n));
    for (int32_t i = 0; i < n; ++i) out.emplace_back(names[i]);
    eb_font_families_free(names, n);
    return out;
}

bool has_family(const std::vector<std::string>& v, const char* name)
{
    return std::find(v.begin(), v.end(), name) != v.end();
}

fs::path epub()
{
    const auto p = first_with_ext(".epub");
    REQUIRE_FALSE(p.empty());
    return p;
}

int32_t body_page(eb_doc* d)
{
    int32_t n = 0;
    REQUIRE(eb_page_count(d, &n) == EB_OK);
    return std::min<int32_t>(n - 1, 8);
}

BitmapPtr render_page(eb_doc* d, int32_t page, const eb_render_opts& o)
{
    eb_bitmap* b = nullptr;
    REQUIRE(eb_render_page(d, page, &o, &b) == EB_OK);
    return BitmapPtr(b);
}

bool same_pixels(const eb_bitmap& a, const eb_bitmap& b)
{
    return a.width == b.width && a.height == b.height &&
           std::memcmp(a.bgra, b.bgra, static_cast<size_t>(a.stride) * static_cast<size_t>(a.height)) == 0;
}

BitmapPtr render_with_css(const char* css, const eb_render_opts& o)
{
    eb_doc* raw = nullptr;
    REQUIRE(eb_open(epub().c_str(), &raw) == EB_OK);
    const DocPtr d(raw);
    REQUIRE(eb_layout(d.get(), 400, 600, 14, css) == EB_OK);
    return render_page(d.get(), body_page(d.get()), o);
}

} // namespace

TEST_CASE("eb_mupdf reflowable pages are cleared to bg_rgb")
{
    const Lib lib;
    const eb_render_opts o{1.0f, 0x000000, 0x112233, 0};
    const BitmapPtr b = render_with_css("html, body { background-color: transparent !important; }", o);
    const uint8_t* corner = b->bgra + static_cast<size_t>(b->stride) * static_cast<size_t>(b->height - 1) + static_cast<size_t>(b->width - 1) * 4;
    CHECK(corner[0] == 0x33);
    CHECK(corner[1] == 0x22);
    CHECK(corner[2] == 0x11);
    CHECK(corner[3] == 255);
    CHECK(all_opaque(*b));
}

TEST_CASE("eb_mupdf reflowable text colour comes from user_css")
{
    const Lib lib;
    const eb_render_opts o{1.0f, 0x00FF00, 0xFFFFFF, 0};
    const BitmapPtr b = render_with_css("* { color: #ff0000 !important; }", o);
    size_t reddish = 0, darkish = 0, greenish = 0;
    for (int32_t y = 0; y < b->height; ++y) {
        const uint8_t* row = b->bgra + static_cast<size_t>(y) * static_cast<size_t>(b->stride);
        for (int32_t x = 0; x < b->width; ++x) {
            const uint8_t* px = row + static_cast<size_t>(x) * 4;
            const uint8_t bl = px[0], g = px[1], r = px[2];
            if (r > 200 && g < 80 && bl < 80) ++reddish;
            if (r < 60 && g < 60 && bl < 60) ++darkish;
            if (g > 200 && r < 80 && bl < 80) ++greenish;
        }
    }
    CHECK(reddish > 50);
    CHECK(darkish == 0);
    CHECK(greenish == 0);
}

TEST_CASE("eb_mupdf tint maps a reflowable page black to fg and white to bg")
{
    const Lib lib;
    const eb_render_opts o{1.0f, 0xD0D0D0, 0x121212, 1};
    const BitmapPtr b = render_with_css(nullptr, o);
    const uint8_t* corner = b->bgra + static_cast<size_t>(b->stride) * static_cast<size_t>(b->height - 1) + static_cast<size_t>(b->width - 1) * 4;
    for (int c = 0; c < 3; ++c) CHECK(std::abs(corner[c] - 0x12) <= 2);
    size_t textlike = 0;
    for (int32_t y = 0; y < b->height; ++y) {
        const uint8_t* row = b->bgra + static_cast<size_t>(y) * static_cast<size_t>(b->stride);
        for (int32_t x = 0; x < b->width; ++x) {
            const uint8_t* px = row + static_cast<size_t>(x) * 4;
            if (std::abs(px[0] - 0xD0) <= 16 && std::abs(px[1] - 0xD0) <= 16 && std::abs(px[2] - 0xD0) <= 16) ++textlike;
        }
    }
    CHECK(textlike > 50);
    CHECK(all_opaque(*b));
}

TEST_CASE("eb_mupdf fixed-layout pages ignore bg_rgb unless tint is set")
{
    const Lib lib;
    const auto p = first_with_ext(".pdf");
    REQUIRE_FALSE(p.empty());
    const DocPtr d = open_doc(p);
    const BitmapPtr white = render_page(d.get(), 0, eb_render_opts{1.0f, 0, 0xFFFFFF, 0});
    const BitmapPtr other = render_page(d.get(), 0, eb_render_opts{1.0f, 0, 0x112233, 0});
    CHECK(same_pixels(*white, *other));
    const BitmapPtr tinted = render_page(d.get(), 0, eb_render_opts{1.0f, 0xFFFFFF, 0x112233, 1});
    CHECK_FALSE(same_pixels(*white, *tinted));
    CHECK(tinted->bgra[0] == 0x33);
    CHECK(tinted->bgra[2] == 0x11);
}

TEST_CASE("eb_mupdf lists system whitelist families sorted and unique")
{
    const Lib lib;
    REQUIRE(set_dirs({system_fonts()}) == EB_OK);
    const auto f = families();
    CHECK((has_family(f, "Georgia") || has_family(f, "Times New Roman")));
    CHECK(std::is_sorted(f.begin(), f.end(), [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; }));
    CHECK(std::adjacent_find(f.begin(), f.end()) == f.end());
    CHECK(set_dirs({}) == EB_OK);
    CHECK(families().empty());
}

TEST_CASE("eb_mupdf font-family in user_css changes the rendering")
{
    const Lib lib;
    REQUIRE(set_dirs({system_fonts()}) == EB_OK);
    const auto f = families();
    if (!has_family(f, "Georgia") || !has_family(f, "Arial")) return;
    const eb_render_opts o{1.0f, 0, 0xFFFFFF, 0};
    const BitmapPtr georgia = render_with_css("* { font-family: \"Georgia\" !important; }", o);
    const BitmapPtr arial = render_with_css("* { font-family: \"arial\" !important; }", o);
    const BitmapPtr unknown = render_with_css("* { font-family: \"No Such Family Zz\" !important; }", o);
    const BitmapPtr generic = render_with_css("* { font-family: serif !important; }", o);
    CHECK(not_uniform(*georgia));
    CHECK(not_uniform(*arial));
    CHECK_FALSE(same_pixels(*georgia, *arial));
    CHECK(not_uniform(*unknown));
    CHECK(same_pixels(*unknown, *generic));
}

TEST_CASE("eb_mupdf font dirs skip corrupt and oversized files")
{
    const Lib lib;
    const TempDir dir;
    {
        std::ofstream bad(dir.path / "corrupt.ttf", std::ios::binary);
        bad << "this is not a font at all, only text";
        std::ofstream big(dir.path / "big.ttf", std::ios::binary);
        const std::string chunk(1 << 20, '\0');
        for (int i = 0; i < 33; ++i) big << chunk;
        const std::ofstream empty(dir.path / "empty.otf", std::ios::binary);
    }
    CHECK(set_dirs({dir.path, dir.path / "missing"}) == EB_OK);
    CHECK(families().empty());
    CHECK(eb_set_font_dirs(nullptr, 1) == EB_E_ARG);
    CHECK(eb_set_font_dirs(nullptr, -1) == EB_E_ARG);
    CHECK(set_dirs({}) == EB_OK);
}

TEST_CASE("eb_mupdf font index startup is cheap")
{
    const Lib lib;
    const fs::path app = EB_FONTS_DIR;
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(set_dirs({app / "Literata", app / "BeVietnamPro", system_fonts()}) == EB_OK);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("eb_set_font_dirs took ", ms, " ms");
    CHECK(ms < ebtest::perf_budget(250));
    const auto f = families();
    CHECK(has_family(f, "Literata"));
    CHECK(has_family(f, "Be Vietnam Pro"));
}

TEST_CASE("eb_mupdf app fonts render Vietnamese text and reject reparse or UNC dirs")
{
    const Lib lib;
    const fs::path app = EB_FONTS_DIR;
    REQUIRE(set_dirs({app / "Literata"}) == EB_OK);
    CHECK(has_family(families(), "Literata"));
    const eb_render_opts o{1.0f, 0, 0xFFFFFF, 0};
    const BitmapPtr lit = render_with_css("* { font-family: \"Literata\" !important; }", o);
    const BitmapPtr ser = render_with_css("* { font-family: \"Times New Roman\" !important; }", o);
    CHECK(not_uniform(*lit));
    CHECK(ser != nullptr);
    CHECK(set_dirs({fs::path(L"\\\\server\\share\\fonts"), fs::path(L"\\\\?\\UNC\\server\\share"), fs::path(L"relative")}) == EB_OK);
    CHECK(families().empty());
}

TEST_CASE("eb_mupdf renders concurrently with fonts registered")
{
    const Lib lib;
    REQUIRE(set_dirs({system_fonts()}) == EB_OK);
    std::atomic<int> failures{0};
    const fs::path book = epub();
    auto work = [&](int id) {
        const char* css = id % 2 ? "* { font-family: \"Georgia\" !important; }" : "* { font-family: serif !important; }";
        eb_doc* raw = nullptr;
        if (eb_open(book.c_str(), &raw) != EB_OK) {
            ++failures;
            return;
        }
        const DocPtr d(raw);
        int32_t n = 0;
        if (eb_layout(d.get(), 400, 600, 14, css) != EB_OK || eb_page_count(d.get(), &n) != EB_OK) ++failures;
        for (int32_t p = 0; p < std::min<int32_t>(n, 6); ++p) {
            const eb_render_opts o{1.0f, 0, 0xFFFFFF, 0};
            eb_bitmap* b = nullptr;
            if (eb_render_page(d.get(), p, &o, &b) != EB_OK) ++failures;
            eb_bitmap_free(b);
        }
    };
    std::vector<std::thread> threads;
    threads.reserve(4);
    for (int i = 0; i < 4; ++i) threads.emplace_back(work, i);
    for (auto& t : threads) t.join();
    CHECK(failures.load() == 0);
}
