#include "eb_test_util.h"

#include "eb_test_epub.h"

#include "../unit/test_base_tmp.h"

#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

constexpr float kW = 400;
constexpr float kH = 600;
constexpr float kEm = 12;
constexpr size_t kChapters = 24;
constexpr size_t kHeader = 48;
constexpr size_t kAccelFixed = 36;

std::string book_epub()
{
    return ebt::multi_chapter_epub({kChapters});
}

void write_bytes(const fs::path& p, const std::string& data)
{
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    REQUIRE(out.good());
}

std::string read_bytes(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct Fixture {
    ebtest::TempDir dir;
    fs::path book = dir.path() / L"book.epub";
    fs::path cache = dir.path() / L"book.lc";
    Fixture() { write_bytes(book, book_epub()); }
};

int32_t pages_of(eb_doc* d)
{
    int32_t n = 0;
    REQUIRE(eb_page_count(d, &n) == EB_OK);
    return n;
}

DocPtr open_with(const Fixture& f, float w = kW, float h = kH, float em = kEm)
{
    eb_doc* raw = nullptr;
    const eb_status st = eb_open_cached(f.book.c_str(), f.cache.c_str(), &raw);
    INFO(eb_status_text(st), ": ", eb_last_error());
    REQUIRE(st == EB_OK);
    DocPtr d(raw);
    REQUIRE(eb_layout(raw, w, h, em, nullptr) == EB_OK);
    return d;
}

int32_t fresh_pages(const Fixture& f, float w, float h, float em)
{
    eb_doc* raw = nullptr;
    REQUIRE(eb_open(f.book.c_str(), &raw) == EB_OK);
    const DocPtr d(raw);
    REQUIRE(eb_layout(raw, w, h, em, nullptr) == EB_OK);
    return pages_of(raw);
}

void save(const Fixture& f)
{
    eb_doc* raw = nullptr;
    REQUIRE(eb_open(f.book.c_str(), &raw) == EB_OK);
    const DocPtr d(raw);
    REQUIRE(eb_layout(raw, kW, kH, kEm, nullptr) == EB_OK);
    REQUIRE(eb_save_cache(raw, f.cache.c_str()) == EB_OK);
}

// First chapter count of the accelerator inside the cache file.
constexpr size_t kFirstCount = kHeader + kAccelFixed;
constexpr size_t kChapterCountField = kHeader + 32;

void patch32(std::string& s, size_t at, uint32_t v)
{
    for (int i = 0; i < 4; ++i) s[at + static_cast<size_t>(i)] = static_cast<char>((v >> (8 * i)) & 0xFFu);
}

} // namespace

TEST_CASE("eb_cache round trip keeps page count and bookmark mapping")
{
    const Lib lib;
    const Fixture f;
    const int32_t truth = fresh_pages(f, kW, kH, kEm);
    REQUIRE(truth > static_cast<int32_t>(kChapters));
    save(f);
    CHECK(fs::file_size(f.cache) == kHeader + kAccelFixed + 4 * kChapters);
    size_t files = 0;
    for (const auto& e : fs::directory_iterator(f.dir.path())) files += e.is_regular_file() ? 1 : 0;
    CHECK(files == 2);
    const DocPtr d = open_with(f);
    CHECK(pages_of(d.get()) == truth);
    eb_bookmark mark = 0;
    int32_t page = -1;
    int32_t again = -1;
    REQUIRE(eb_bookmark_of_page(d.get(), truth / 2, &mark) == EB_OK);
    REQUIRE(eb_page_of_bookmark(d.get(), mark, &page) == EB_OK);
    CHECK(page == truth / 2);
    eb_doc* raw = nullptr;
    REQUIRE(eb_open(f.book.c_str(), &raw) == EB_OK);
    const DocPtr uncached(raw);
    REQUIRE(eb_layout(raw, kW, kH, kEm, nullptr) == EB_OK);
    REQUIRE(eb_page_of_bookmark(raw, mark, &again) == EB_OK);
    CHECK(again == page);
    eb_bitmap* bmp = nullptr;
    const eb_render_opts opts{1.0f, 0x000000, 0xFFFFFF, 1};
    REQUIRE(eb_render_page(d.get(), truth - 1, &opts, &bmp) == EB_OK);
    eb_bitmap_free(bmp);
}

TEST_CASE("eb_cache is really used: a valid cache with an edited count drives the page count")
{
    const Lib lib;
    const Fixture f;
    const int32_t truth = fresh_pages(f, kW, kH, kEm);
    save(f);
    std::string bytes = read_bytes(f.cache);
    uint32_t first = 0;
    std::memcpy(&first, bytes.data() + kFirstCount, 4);
    patch32(bytes, kFirstCount, first + 3);
    write_bytes(f.cache, bytes);
    CHECK(pages_of(open_with(f).get()) == truth + 3);
}

TEST_CASE("eb_cache is invalidated when the registered fonts change")
{
    const Lib lib;
    const Fixture f;
    const fs::path arial = L"C:\\Windows\\Fonts\\arial.ttf";
    REQUIRE(fs::exists(arial));
    const int32_t truth = fresh_pages(f, kW, kH, kEm);
    save(f);
    std::string bytes = read_bytes(f.cache);
    uint32_t first = 0;
    std::memcpy(&first, bytes.data() + kFirstCount, 4);
    patch32(bytes, kFirstCount, first + 3);
    write_bytes(f.cache, bytes);
    REQUIRE(pages_of(open_with(f).get()) == truth + 3);
    const fs::path fonts = f.dir.path() / L"fonts";
    fs::create_directories(fonts);
    fs::copy_file(arial, fonts / L"arial.ttf");
    const wchar_t* dirs[] = {fonts.c_str()};
    REQUIRE(eb_set_font_dirs(dirs, 1) == EB_OK);
    CHECK(pages_of(open_with(f).get()) != truth + 3);
    REQUIRE(eb_set_font_dirs(nullptr, 0) == EB_OK);
    CHECK(pages_of(open_with(f).get()) == truth + 3);
}

TEST_CASE("eb_cache for another layout is discarded by the engine")
{
    const Lib lib;
    const Fixture f;
    save(f);
    for (const auto& [w, h, em] : {std::tuple<float, float, float>{300.0f, 500.0f, 14.0f}, {400.0f, 600.0f, 18.0f}, {800.0f, 300.0f, 12.0f}}) {
        const DocPtr d = open_with(f, w, h, em);
        CHECK(pages_of(d.get()) == fresh_pages(f, w, h, em));
    }
}

TEST_CASE("eb_cache damaged files are ignored")
{
    const Lib lib;
    const Fixture f;
    const int32_t truth = fresh_pages(f, kW, kH, kEm);
    save(f);
    const std::string good = read_bytes(f.cache);
    std::vector<std::pair<const char*, std::string>> bad;
    bad.emplace_back("empty", std::string());
    bad.emplace_back("header only", good.substr(0, kHeader));
    bad.emplace_back("truncated", good.substr(0, good.size() - 5));
    bad.emplace_back("trailing byte", good + "x");
    bad.emplace_back("oversized", good + std::string(2u << 20, '\0'));
    bad.emplace_back("junk", std::string(300, 'z'));
    for (const size_t at : {size_t{0}, size_t{4}, size_t{8}, size_t{16}, size_t{24}, size_t{32}, size_t{40}, kHeader, kHeader + 4, kHeader + 8}) {
        std::string s = good;
        s[at] = static_cast<char>(s[at] ^ 0x55);
        bad.emplace_back("flipped header byte", s);
    }
    for (const uint32_t v : {0x7fffffffu, 0x80000000u, 0xfffffffeu, 100000u + 1u}) {
        std::string s = good;
        patch32(s, kFirstCount, v);
        bad.emplace_back("huge page count", s);
    }
    for (const uint32_t v : {0u, 1u, 100001u, 0xffffffffu, 0x7fffffffu}) {
        std::string s = good;
        patch32(s, kChapterCountField, v);
        bad.emplace_back("wrong chapter count", s);
    }
    for (const auto& [name, data] : bad) {
        INFO(name);
        write_bytes(f.cache, data);
        const DocPtr d = open_with(f);
        CHECK(pages_of(d.get()) == truth);
    }
}

TEST_CASE("eb_cache missing path and unwritable directory")
{
    const Lib lib;
    const Fixture f;
    fs::remove(f.cache);
    CHECK(pages_of(open_with(f).get()) == fresh_pages(f, kW, kH, kEm));
    eb_doc* raw = nullptr;
    REQUIRE(eb_open_cached(f.book.c_str(), nullptr, &raw) == EB_OK);
    const DocPtr d(raw);
    REQUIRE(eb_layout(raw, kW, kH, kEm, nullptr) == EB_OK);
    CHECK(eb_save_cache(raw, nullptr) == EB_E_ARG);
    const fs::path nowhere = f.dir.path() / L"missing" / L"x.lc";
    CHECK(eb_save_cache(raw, nowhere.c_str()) == EB_E_OPEN);
}

TEST_CASE("eb_cache is ignored when the book file changed")
{
    const Lib lib;
    const Fixture f;
    const int32_t truth = fresh_pages(f, kW, kH, kEm);
    save(f);
    std::string bytes = read_bytes(f.cache);
    uint32_t first = 0;
    std::memcpy(&first, bytes.data() + kFirstCount, 4);
    patch32(bytes, kFirstCount, first + 3);
    write_bytes(f.cache, bytes);
    REQUIRE(pages_of(open_with(f).get()) == truth + 3);
    fs::last_write_time(f.book, fs::last_write_time(f.book) + std::chrono::seconds(7));
    CHECK(pages_of(open_with(f).get()) == truth);
    fs::last_write_time(f.book, fs::last_write_time(f.book) - std::chrono::seconds(7));
    CHECK(pages_of(open_with(f).get()) == truth + 3);
    write_bytes(f.book, book_epub() + std::string(16, '\0'));
    fs::last_write_time(f.book, fs::last_write_time(f.cache));
    CHECK(pages_of(open_with(f).get()) == truth);
}

TEST_CASE("eb_save_cache returns EB_E_FORMAT where no chapter cache exists")
{
    const Lib lib;
    const ebtest::TempDir dir;
    const std::string pdf =
        "%PDF-1.4\n1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1>>endobj\n"
        "3 0 obj<</Type/Page/Parent 2 0 R/MediaBox[0 0 200 200]>>endobj\ntrailer<</Root 1 0 R>>\n%%EOF\n";
    const fs::path pdf_path = dir.path() / L"a.pdf";
    write_bytes(pdf_path, pdf);
    const fs::path cache = dir.path() / L"a.lc";
    eb_doc* raw = nullptr;
    REQUIRE(eb_open_cached(pdf_path.c_str(), cache.c_str(), &raw) == EB_OK);
    const DocPtr d(raw);
    CHECK(eb_save_cache(raw, cache.c_str()) == EB_E_FORMAT);
    CHECK_FALSE(fs::exists(cache));
    const std::string epub = book_epub();
    eb_doc* mem = nullptr;
    REQUIRE(eb_open_memory(epub.data(), epub.size(), ".epub", &mem) == EB_OK);
    const DocPtr m(mem);
    REQUIRE(eb_layout(mem, kW, kH, kEm, nullptr) == EB_OK);
    CHECK(eb_save_cache(mem, cache.c_str()) == EB_E_FORMAT);
    CHECK_FALSE(fs::exists(cache));
}
