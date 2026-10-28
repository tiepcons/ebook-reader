#include "eb_mupdf.h"

#include <cstddef>
#include <cstdint>
#include <iterator>

namespace {

constexpr const char* kMagic[] = {"epub", "fb2", "cbz", "pdf", "mobi"};

struct DocGuard {
    eb_doc* doc = nullptr;
    ~DocGuard() { eb_close(doc); }
};

void exercise(eb_doc* doc)
{
    if (eb_is_reflowable(doc) && eb_layout(doc, 360.f, 560.f, 12.f, nullptr) != EB_OK) return;
    int32_t pages = 0;
    if (eb_page_count(doc, &pages) != EB_OK || pages <= 0) return;
    const eb_render_opts opts{1.0f, 0x202020, 0xF0F0E0, 1};
    eb_bitmap* bmp = nullptr;
    if (eb_render_page(doc, 0, &opts, &bmp) == EB_OK) eb_bitmap_free(bmp);
    eb_text* text = nullptr;
    if (eb_page_text(doc, 0, &text) == EB_OK) eb_text_free(text);
    eb_toc_item* items = nullptr;
    int32_t count = 0;
    if (eb_toc(doc, &items, &count) == EB_OK) eb_toc_free(items, count);
    eb_link* links = nullptr;
    if (eb_page_links(doc, 0, &links, &count) == EB_OK) eb_links_free(links);
    char buf[64];
    size_t needed = 0;
    eb_meta(doc, "title", buf, sizeof buf, &needed);
    eb_meta(doc, "author", buf, sizeof buf, &needed);
    eb_bookmark mark = 0;
    int32_t page = 0;
    if (eb_bookmark_of_page(doc, pages - 1, &mark) == EB_OK) eb_page_of_bookmark(doc, mark, &page);
    if (eb_render_cover(doc, 128, &bmp) == EB_OK) eb_bitmap_free(bmp);
}

}

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    const eb_limits limits{64u << 20, 5000, 4u << 20, size_t{16} << 20, uint64_t{768} << 20};
    return eb_init(&limits) == EB_OK ? 0 : 1;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < 2) return 0;
    DocGuard g;
    if (eb_open_memory(data + 1, size - 1, kMagic[data[0] % std::size(kMagic)], &g.doc) != EB_OK) return 0;
    exercise(g.doc);
    return 0;
}
