#ifndef EB_TEST_UTIL_H
#define EB_TEST_UTIL_H

#include <doctest/doctest.h>
#include <eb_mupdf.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace ebt {


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

inline std::vector<fs::path> corpus(const char* sub)
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

inline fs::path first_with_ext(const char* ext)
{
    for (const auto& f : corpus("good")) {
        if (f.extension() == ext) return f;
    }
    return {};
}

inline bool has_image_ext(const fs::path& p)
{
    const std::string e = p.extension().string();
    for (const char* x : {".cbz", ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".tif", ".tiff"}) {
        if (e == x) return true;
    }
    return false;
}

inline DocPtr open_doc(const fs::path& p)
{
    eb_doc* d = nullptr;
    const eb_status st = eb_open(p.c_str(), &d);
    INFO(p.string(), " -> ", eb_status_text(st), ": ", eb_last_error());
    REQUIRE(st == EB_OK);
    DocPtr doc(d);
    REQUIRE(eb_layout(d, 400, 600, 12, nullptr) == EB_OK);
    return doc;
}

inline bool not_uniform(const eb_bitmap& b)
{
    for (int32_t y = 0; y < b.height; ++y) {
        const uint8_t* row = b.bgra + static_cast<size_t>(y) * static_cast<size_t>(b.stride);
        for (int32_t x = 0; x < b.width * 4; x += 4) {
            if (row[x] != 255 || row[x + 1] != 255 || row[x + 2] != 255) return true;
        }
    }
    return false;
}

inline bool all_opaque(const eb_bitmap& b)
{
    for (int32_t y = 0; y < b.height; ++y) {
        const uint8_t* row = b.bgra + static_cast<size_t>(y) * static_cast<size_t>(b.stride);
        for (int32_t x = 3; x < b.width * 4; x += 4) {
            if (row[x] != 255) return false;
        }
    }
    return true;
}

inline BitmapPtr render0(eb_doc* d, const eb_render_opts& o)
{
    eb_bitmap* b = nullptr;
    REQUIRE(eb_render_page(d, 0, &o, &b) == EB_OK);
    REQUIRE(b != nullptr);
    return BitmapPtr(b);
}

} // namespace ebt

using namespace ebt;

#endif
