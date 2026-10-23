#include <doctest/doctest.h>
#include <eb_mupdf.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Lib {
    explicit Lib(const eb_limits* limits = nullptr) { REQUIRE(eb_init(limits) == EB_OK); }
    Lib(const Lib&) = delete;
    Lib& operator=(const Lib&) = delete;
    ~Lib() { eb_shutdown(); }
};

struct DocDeleter {
    void operator()(eb_doc* d) const { eb_close(d); }
};
struct BitmapDeleter {
    void operator()(eb_bitmap* b) const { eb_bitmap_free(b); }
};
struct TextDeleter {
    void operator()(eb_text* t) const { eb_text_free(t); }
};
using DocPtr = std::unique_ptr<eb_doc, DocDeleter>;
using BitmapPtr = std::unique_ptr<eb_bitmap, BitmapDeleter>;
using TextPtr = std::unique_ptr<eb_text, TextDeleter>;

std::vector<fs::path> corpus(const char* sub)
{
    std::vector<fs::path> out;
    std::error_code ec;
    const fs::path dir = fs::path(EB_CORPUS_DIR) / sub;
    if (!fs::is_directory(dir, ec)) return out;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool has_image_ext(const fs::path& p)
{
    const std::string e = p.extension().string();
    for (const char* x : {".cbz", ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".tif", ".tiff"}) {
        if (e == x) return true;
    }
    return false;
}

DocPtr open_doc(const fs::path& p)
{
    eb_doc* d = nullptr;
    const eb_status st = eb_open(p.c_str(), &d);
    INFO(p.string(), " -> ", eb_status_text(st), ": ", eb_last_error());
    REQUIRE(st == EB_OK);
    return DocPtr(d);
}

bool not_uniform(const eb_bitmap& b)
{
    for (int32_t y = 0; y < b.height; ++y) {
        const uint8_t* row = b.bgra + static_cast<size_t>(y) * static_cast<size_t>(b.stride);
        for (int32_t x = 0; x < b.width * 4; x += 4) {
            if (row[x] != 255 || row[x + 1] != 255 || row[x + 2] != 255) return true;
        }
    }
    return false;
}

bool all_opaque(const eb_bitmap& b)
{
    for (int32_t y = 0; y < b.height; ++y) {
        const uint8_t* row = b.bgra + static_cast<size_t>(y) * static_cast<size_t>(b.stride);
        for (int32_t x = 3; x < b.width * 4; x += 4) {
            if (row[x] != 255) return false;
        }
    }
    return true;
}

BitmapPtr render0(eb_doc* d, const eb_render_opts& o)
{
    eb_bitmap* b = nullptr;
    REQUIRE(eb_render_page(d, 0, &o, &b) == EB_OK);
    REQUIRE(b != nullptr);
    return BitmapPtr(b);
}

bool any_page_has_text(eb_doc* d, int32_t count)
{
    for (int32_t p = 0; p < std::min<int32_t>(count, 20); ++p) {
        eb_text* raw = nullptr;
        REQUIRE(eb_page_text(d, p, &raw) == EB_OK);
        const TextPtr t(raw);
        if (t->utf8_len > 0) {
            CHECK(t->char_count > 0);
            CHECK(std::strlen(t->utf8) == t->utf8_len);
            CHECK(t->utf8_offset[0] == 0);
            return true;
        }
    }
    return false;
}

void check_bookmarks(eb_doc* d, int32_t count)
{
    const int32_t k = count / 2;
    eb_bookmark bm = 0;
    REQUIRE(eb_bookmark_of_page(d, k, &bm) == EB_OK);
    REQUIRE(eb_layout(d, 400, 600, 18, nullptr) == EB_OK);
    int32_t after = -1;
    int32_t n2 = 0;
    REQUIRE(eb_page_count(d, &n2) == EB_OK);
    REQUIRE(eb_page_of_bookmark(d, bm, &after) == EB_OK);
    CHECK(after >= 0);
    CHECK(after < n2);
    eb_bookmark bm2 = 0;
    int32_t again = -1;
    REQUIRE(eb_bookmark_of_page(d, after, &bm2) == EB_OK);
    REQUIRE(eb_page_of_bookmark(d, bm2, &again) == EB_OK);
    CHECK(again == after);
}

} // namespace

TEST_CASE("eb_mupdf reports ABI and engine versions")
{
    CHECK(eb_abi_version() == EB_ABI_VERSION);
    CHECK(std::strlen(eb_engine_version()) > 0);
}

TEST_CASE("eb_mupdf opens, lays out, renders and reads every good corpus file")
{
    const Lib lib;
    const auto files = corpus("good");
    REQUIRE_FALSE(files.empty());
    for (const auto& p : files) {
        INFO(p.string());
        const DocPtr d = open_doc(p);
        REQUIRE(eb_layout(d.get(), 400, 600, 12, nullptr) == EB_OK);
        int32_t count = 0;
        REQUIRE(eb_page_count(d.get(), &count) == EB_OK);
        REQUIRE(count > 0);

        const eb_render_opts plain{1.0f, 0, 0xFFFFFF, 0};
        const BitmapPtr bmp = render0(d.get(), plain);
        CHECK(bmp->width > 0);
        CHECK(bmp->stride == bmp->width * 4);
        CHECK(not_uniform(*bmp));
        CHECK(all_opaque(*bmp));

        if (!has_image_ext(p)) CHECK(any_page_has_text(d.get(), count));

        char buf[64] = {};
        size_t needed = 0;
        REQUIRE(eb_meta(d.get(), "format", buf, sizeof buf, &needed) == EB_OK);
        CHECK(buf[0] != '\0');
        CHECK(needed == std::strlen(buf) + 1);

        eb_toc_item* items = nullptr;
        int32_t n = -1;
        REQUIRE(eb_toc(d.get(), &items, &n) == EB_OK);
        CHECK(n >= 0);
        for (int32_t i = 0; i < n; ++i) CHECK(items[i].title != nullptr);
        eb_toc_free(items, n);

        float w = 0;
        float h = 0;
        REQUIRE(eb_page_size(d.get(), 0, &w, &h) == EB_OK);
        CHECK(w > 0);
        CHECK(h > 0);
        check_bookmarks(d.get(), count);
    }
}

TEST_CASE("eb_mupdf rejects bad corpus files without crashing")
{
    const Lib lib;
    for (const auto& p : corpus("bad")) {
        INFO(p.string());
        eb_doc* raw = nullptr;
        const eb_status st = eb_open(p.c_str(), &raw);
        const DocPtr d(raw);
        if (st != EB_OK) continue;
        eb_bitmap* b = nullptr;
        const eb_render_opts o{1.0f, 0, 0xFFFFFF, 0};
        const eb_status rs = eb_render_page(d.get(), 0, &o, &b);
        const BitmapPtr guard(b);
        const std::string name = p.filename().string();
        const bool tolerated = name == "badxml.fb2" || name == "truncated.mobi" || name == "zipbomb-ish.cbz";
        if (!tolerated) CHECK(rs != EB_OK);
    }
}

TEST_CASE("eb_mupdf enforces limits and argument checks")
{
    const auto files = corpus("good");
    REQUIRE_FALSE(files.empty());
    {
        const eb_limits tiny{10, 0, 0, 0};
        const Lib lib(&tiny);
        eb_doc* d = nullptr;
        CHECK(eb_open(files[0].c_str(), &d) == EB_E_LIMIT);
        CHECK(d == nullptr);
    }
    const Lib lib;
    eb_doc* none = nullptr;
    CHECK(eb_open(L"does-not-exist.epub", &none) == EB_E_OPEN);
    CHECK(eb_open(nullptr, &none) == EB_E_ARG);

    const DocPtr d = open_doc(files[0]);
    const eb_render_opts huge{1000.0f, 0, 0xFFFFFF, 0};
    eb_bitmap* b = nullptr;
    CHECK(eb_render_page(d.get(), 0, &huge, &b) == EB_E_LIMIT);
    CHECK(b == nullptr);
    const eb_render_opts ok{1.0f, 0, 0xFFFFFF, 0};
    CHECK(eb_render_page(d.get(), 100000, &ok, &b) == EB_E_RANGE);
    char buf[8] = {};
    size_t needed = 0;
    CHECK(eb_meta(d.get(), "language", buf, sizeof buf, &needed) == EB_E_RANGE);
}

TEST_CASE("eb_mupdf tint changes the rendered pixels")
{
    const Lib lib;
    const auto files = corpus("good");
    REQUIRE_FALSE(files.empty());
    const DocPtr d = open_doc(files[0]);
    const BitmapPtr plain = render0(d.get(), eb_render_opts{1.0f, 0, 0xFFFFFF, 0});
    const BitmapPtr tinted = render0(d.get(), eb_render_opts{1.0f, 0xE0E0E0, 0x101010, 1});
    REQUIRE(plain->width == tinted->width);
    REQUIRE(plain->height == tinted->height);
    const size_t bytes = static_cast<size_t>(plain->stride) * static_cast<size_t>(plain->height);
    CHECK(std::memcmp(plain->bgra, tinted->bgra, bytes) != 0);
    CHECK(all_opaque(*tinted));
}

TEST_CASE("eb_mupdf cover fits the requested side and memory open works")
{
    const Lib lib;
    const auto files = corpus("good");
    REQUIRE_FALSE(files.empty());
    const DocPtr d = open_doc(files[0]);
    eb_bitmap* raw = nullptr;
    REQUIRE(eb_render_cover(d.get(), 200, &raw) == EB_OK);
    const BitmapPtr cover(raw);
    CHECK(std::max(cover->width, cover->height) == 200);

    std::ifstream in(files[0], std::ios::binary);
    const std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE_FALSE(data.empty());
    eb_doc* m = nullptr;
    const std::string magic = files[0].extension().string();
    REQUIRE(eb_open_memory(data.data(), data.size(), magic.c_str(), &m) == EB_OK);
    const DocPtr mem(m);
    int32_t count = 0;
    CHECK(eb_page_count(mem.get(), &count) == EB_OK);
    CHECK(count > 0);

    const char junk[] = "this is not a document";
    eb_doc* bad = nullptr;
    CHECK(eb_open_memory(junk, sizeof junk, ".epub", &bad) != EB_OK);
    CHECK(bad == nullptr);
}
