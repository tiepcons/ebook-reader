#include "eb_test_util.h"

#include "eb_test_epub.h"

#include "../unit/test_base_tmp.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <thread>

namespace {

constexpr float kW = 300;
constexpr float kH = 200;
constexpr float kEm = 12;
constexpr size_t kChapters = 24;

eb_bookmark chapter_mark(size_t chapter)
{
    return static_cast<eb_bookmark>(chapter) << 32;
}

DocPtr open_epub(const std::string& epub)
{
    eb_doc* raw = nullptr;
    REQUIRE(eb_open_memory(epub.data(), epub.size(), ".epub", &raw) == EB_OK);
    return DocPtr(raw);
}

int32_t count_of(eb_doc* d)
{
    int32_t n = 0;
    REQUIRE(eb_page_count(d, &n) == EB_OK);
    return n;
}

int32_t page_of(eb_doc* d, eb_bookmark m)
{
    int32_t p = -1;
    REQUIRE(eb_page_of_bookmark(d, m, &p) == EB_OK);
    return p;
}

// Eager layout of the same book: the numbering a lazy layout must converge to.
struct Truth {
    DocPtr doc;
    std::vector<int32_t> start;
    int32_t total = 0;
    explicit Truth(const std::string& epub, size_t chapters = kChapters) : doc(open_epub(epub))
    {
        REQUIRE(eb_layout(doc.get(), kW, kH, kEm, nullptr) == EB_OK);
        total = count_of(doc.get());
        for (size_t c = 0; c < chapters; ++c) start.push_back(page_of(doc.get(), chapter_mark(c)));
    }
    [[nodiscard]] int32_t pages_in(size_t c) const { return (c + 1 < start.size() ? start[c + 1] : total) - start[c]; }
};

// Steps until complete; returns how many steps it took.
int run_steps(eb_doc* d, eb_bookmark focus_at)
{
    int32_t complete = 0;
    int steps = 0;
    while (!complete && steps < 1000) {
        REQUIRE(eb_layout_step(d, focus_at, &complete) == EB_OK);
        ++steps;
    }
    REQUIRE(complete == 1);
    return steps;
}

} // namespace

TEST_CASE("eb_layout_lazy lays out only the chapter that is asked for and converges to the eager numbering")
{
    const Lib lib;
    const std::string epub = ebt::multi_chapter_epub({kChapters});
    const Truth truth(epub);
    REQUIRE(truth.total > static_cast<int32_t>(kChapters) * 2);
    const DocPtr d = open_epub(epub);
    REQUIRE(eb_layout_lazy(d.get(), kW, kH, kEm, nullptr) == EB_OK);
    CHECK(count_of(d.get()) == static_cast<int32_t>(kChapters));
    const size_t focus = 10;
    const int32_t page = page_of(d.get(), chapter_mark(focus));
    eb_bookmark mark = 0;
    REQUIRE(eb_bookmark_of_page(d.get(), page, &mark) == EB_OK);
    CHECK(mark == chapter_mark(focus));
    eb_bitmap* bmp = nullptr;
    const eb_render_opts opts{1.0f, 0x000000, 0xFFFFFF, 1};
    REQUIRE(eb_render_page(d.get(), page, &opts, &bmp) == EB_OK);
    eb_bitmap_free(bmp);
    CHECK(count_of(d.get()) != truth.total);
    CHECK(run_steps(d.get(), chapter_mark(focus)) == static_cast<int>(kChapters) - 1);
    CHECK(count_of(d.get()) == truth.total);
    for (size_t c = 0; c < kChapters; ++c) CHECK(page_of(d.get(), chapter_mark(c)) == truth.start[c]);
    const uint32_t gen = eb_layout_generation(d.get());
    int32_t complete = 0;
    REQUIRE(eb_layout_step(d.get(), 0, &complete) == EB_OK);
    CHECK(complete == 1);
    CHECK(eb_layout_generation(d.get()) == gen);
}

TEST_CASE("eb_layout_step lays out the chapters after focus_at first, then the ones before it")
{
    const Lib lib;
    const std::string epub = ebt::multi_chapter_epub({kChapters});
    const Truth truth(epub);
    const size_t focus_at = 5;
    const DocPtr d = open_epub(epub);
    REQUIRE(eb_layout_lazy(d.get(), kW, kH, kEm, nullptr) == EB_OK);
    int32_t complete = 0;
    for (size_t i = 0; i < kChapters - focus_at; ++i) REQUIRE(eb_layout_step(d.get(), chapter_mark(focus_at) + 7, &complete) == EB_OK);
    CHECK(complete == 0);
    // Chapters 0..focus_at-1 are still estimates, so everything from `focus_at` on is shifted by their error only.
    for (size_t c = focus_at + 1; c < kChapters; ++c) CHECK(page_of(d.get(), chapter_mark(c)) - page_of(d.get(), chapter_mark(focus_at)) == truth.start[c] - truth.start[focus_at]);
    for (size_t i = 0; i < focus_at; ++i) REQUIRE(eb_layout_step(d.get(), chapter_mark(focus_at) + 7, &complete) == EB_OK);
    CHECK(complete == 1);
    CHECK(page_of(d.get(), chapter_mark(focus_at)) == truth.start[focus_at]);
    CHECK(count_of(d.get()) == truth.total);
}

TEST_CASE("eb_layout_generation changes only when an estimate was wrong")
{
    const Lib lib;
    const std::string epub = ebt::multi_chapter_epub({kChapters, true});
    const Truth truth(epub);
    const DocPtr d = open_epub(epub);
    REQUIRE(eb_layout_lazy(d.get(), kW, kH, kEm, nullptr) == EB_OK);
    const uint32_t g0 = eb_layout_generation(d.get());
    const int32_t c0 = count_of(d.get());
    (void)page_of(d.get(), chapter_mark(3));
    const uint32_t g1 = eb_layout_generation(d.get());
    CHECK(g1 != g0);
    CHECK(count_of(d.get()) != c0);
    int32_t complete = 0;
    const int32_t last = count_of(d.get());
    while (!complete) {
        REQUIRE(eb_layout_step(d.get(), chapter_mark(3), &complete) == EB_OK);
        CHECK(eb_layout_generation(d.get()) == g1);
        CHECK(count_of(d.get()) == last);
    }
    CHECK(count_of(d.get()) == truth.total);
}

TEST_CASE("eb_layout_generation follows every renumbering on the way to the exact count")
{
    const Lib lib;
    const std::string epub = ebt::multi_chapter_epub({kChapters});
    const DocPtr d = open_epub(epub);
    REQUIRE(eb_layout_lazy(d.get(), kW, kH, kEm, nullptr) == EB_OK);
    int32_t complete = 0;
    int bumps = 0;
    while (!complete) {
        const uint32_t g = eb_layout_generation(d.get());
        const int32_t n = count_of(d.get());
        REQUIRE(eb_layout_step(d.get(), 0, &complete) == EB_OK);
        const bool bumped = eb_layout_generation(d.get()) != g;
        bumps += bumped ? 1 : 0;
        if (count_of(d.get()) != n) CHECK(bumped);
        if (!bumped) CHECK(count_of(d.get()) == n);
    }
    CHECK(bumps > 0);
}

TEST_CASE("a page number in a chapter not laid out yet keeps its fraction of the chapter")
{
    const Lib lib;
    const std::string epub = ebt::multi_chapter_epub({kChapters});
    const Truth truth(epub);
    const DocPtr d = open_epub(epub);
    REQUIRE(eb_layout_lazy(d.get(), kW, kH, kEm, nullptr) == EB_OK);
    size_t anchor = 0;
    while (truth.pages_in(anchor) < 3) ++anchor;
    (void)page_of(d.get(), chapter_mark(anchor));
    const int32_t estimate = truth.pages_in(anchor);
    size_t target = anchor + 1;
    while (truth.pages_in(target) == estimate || truth.pages_in(target) < 2) ++target;
    const uint32_t gen = eb_layout_generation(d.get());
    const auto page = static_cast<int32_t>(target) * estimate + (estimate - 1);
    eb_bookmark mark = 0;
    REQUIRE(eb_bookmark_of_page(d.get(), page, &mark) == EB_OK);
    CHECK((mark >> 32) == target);
    CHECK(eb_layout_generation(d.get()) != gen);
    const int32_t real = truth.pages_in(target);
    const auto expected = std::min(real - 1, static_cast<int32_t>(std::floor(static_cast<double>(estimate - 1) / estimate * real + 0.5)));
    const int32_t start = page_of(d.get(), chapter_mark(target));
    CHECK(page_of(d.get(), mark) - start == expected);
}

TEST_CASE("render, text, links and page size lay out the chapter of the page they are given")
{
    const Lib lib;
    const std::string epub = ebt::multi_chapter_epub({kChapters});
    const DocPtr d = open_epub(epub);
    REQUIRE(eb_layout_lazy(d.get(), kW, kH, kEm, nullptr) == EB_OK);
    float w = 0, h = 0;
    REQUIRE(eb_page_size(d.get(), count_of(d.get()) - 1, &w, &h) == EB_OK);
    CHECK(w > 0);
    eb_text* text = nullptr;
    REQUIRE(eb_page_text(d.get(), count_of(d.get()) - 1, &text) == EB_OK);
    const TextPtr t(text);
    CHECK(t->char_count > 0);
    eb_link* links = nullptr;
    int32_t n = 0;
    REQUIRE(eb_page_links(d.get(), count_of(d.get()) - 1, &links, &n) == EB_OK);
    eb_links_free(links);
    CHECK(eb_page_text(d.get(), count_of(d.get()), &text) == EB_E_RANGE);
    CHECK(eb_page_size(d.get(), -1, &w, &h) == EB_E_RANGE);
}

TEST_CASE("eb_layout_step can be aborted and loses nothing")
{
    const Lib lib;
    const std::string epub = ebt::multi_chapter_epub({kChapters});
    const Truth truth(epub);
    const DocPtr d = open_epub(epub);
    REQUIRE(eb_layout_lazy(d.get(), kW, kH, kEm, nullptr) == EB_OK);
    std::atomic<bool> stop{false};
    std::thread hammer([&] {
        while (!stop) eb_abort(d.get());
    });
    int aborted = 0;
    int32_t complete = 0;
    for (int i = 0; i < 200 && !complete; ++i) {
        const eb_status st = eb_layout_step(d.get(), 0, &complete);
        if (st == EB_E_ABORTED) ++aborted;
        else REQUIRE(st == EB_OK);
    }
    stop = true;
    hammer.join();
    CHECK(aborted > 0);
    run_steps(d.get(), 0);
    CHECK(count_of(d.get()) == truth.total);
    for (size_t c = 0; c < kChapters; ++c) CHECK(page_of(d.get(), chapter_mark(c)) == truth.start[c]);
}

TEST_CASE("eb_toc in a lazy layout does not lay out the book")
{
    const Lib lib;
    const std::string epub = ebt::multi_chapter_epub({kChapters, false, true});
    const DocPtr d = open_epub(epub);
    REQUIRE(eb_layout_lazy(d.get(), kW, kH, kEm, nullptr) == EB_OK);
    const uint32_t gen = eb_layout_generation(d.get());
    eb_toc_item* items = nullptr;
    int32_t n = 0;
    REQUIRE(eb_toc(d.get(), &items, &n) == EB_OK);
    REQUIRE(n == static_cast<int32_t>(kChapters / 3));
    for (int32_t i = 0; i < n; ++i) {
        CHECK((items[i].target >> 32) == static_cast<uint64_t>(i) * 3);
        CHECK(items[i].page == i * 3);
    }
    eb_toc_free(items, n);
    CHECK(eb_layout_generation(d.get()) == gen);
    CHECK(run_steps(d.get(), 0) == static_cast<int>(kChapters));
}

TEST_CASE("fixed-layout and single-flow documents ignore the progressive layout")
{
    const Lib lib;
    const std::string pdf =
        "%PDF-1.4\n1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1>>endobj\n"
        "3 0 obj<</Type/Page/Parent 2 0 R/MediaBox[0 0 200 200]>>endobj\ntrailer<</Root 1 0 R>>\n%%EOF\n";
    eb_doc* raw = nullptr;
    REQUIRE(eb_open_memory(pdf.data(), pdf.size(), ".pdf", &raw) == EB_OK);
    const DocPtr p(raw);
    REQUIRE(eb_layout_lazy(raw, kW, kH, kEm, nullptr) == EB_OK);
    int32_t complete = 0;
    REQUIRE(eb_layout_step(raw, 0, &complete) == EB_OK);
    CHECK(complete == 1);
    CHECK(count_of(raw) == 1);
    const std::string fb2 =
        R"(<?xml version="1.0" encoding="utf-8"?><FictionBook xmlns="http://www.gribuser.ru/xml/fictionbook/2.0">)"
        R"(<description><title-info><genre>prose</genre><book-title>Lazy</book-title></title-info></description>)"
        R"(<body><section><p>Hello lazy world.</p></section></body></FictionBook>)";
    REQUIRE(eb_open_memory(fb2.data(), fb2.size(), ".fb2", &raw) == EB_OK);
    const DocPtr f(raw);
    REQUIRE(eb_layout(raw, kW, kH, kEm, nullptr) == EB_OK);
    const int32_t eager = count_of(raw);
    REQUIRE(eb_layout_lazy(raw, kW, kH, kEm, nullptr) == EB_OK);
    CHECK(count_of(raw) == eager);
    REQUIRE(eb_layout_step(raw, 0, &complete) == EB_OK);
    CHECK(complete == 1);
    CHECK(eb_layout_step(raw, 0, nullptr) == EB_E_ARG);
}

TEST_CASE("a layout cache makes the lazy layout exact at once and a partial layout can be saved")
{
    const Lib lib;
    const ebtest::TempDir dir;
    const fs::path book = dir.path() / L"book.epub";
    const fs::path cache = dir.path() / L"book.lc";
    const std::string epub = ebt::multi_chapter_epub({kChapters});
    {
        std::ofstream out(book, std::ios::binary);
        out.write(epub.data(), static_cast<std::streamsize>(epub.size()));
        REQUIRE(out.good());
    }
    const Truth truth(epub);
    const auto open_cached = [&] {
        eb_doc* raw = nullptr;
        REQUIRE(eb_open_cached(book.c_str(), cache.c_str(), &raw) == EB_OK);
        return DocPtr(raw);
    };
    {
        const DocPtr d = open_cached();
        REQUIRE(eb_layout_lazy(d.get(), kW, kH, kEm, nullptr) == EB_OK);
        (void)page_of(d.get(), chapter_mark(7));
        REQUIRE(eb_save_cache(d.get(), cache.c_str()) == EB_OK);
    }
    {
        const DocPtr d = open_cached();
        REQUIRE(eb_layout_lazy(d.get(), kW, kH, kEm, nullptr) == EB_OK);
        CHECK(run_steps(d.get(), 0) == static_cast<int>(kChapters) - 1);
        CHECK(count_of(d.get()) == truth.total);
        REQUIRE(eb_save_cache(d.get(), cache.c_str()) == EB_OK);
    }
    const DocPtr d = open_cached();
    REQUIRE(eb_layout_lazy(d.get(), kW, kH, kEm, nullptr) == EB_OK);
    CHECK(count_of(d.get()) == truth.total);
    const uint32_t gen = eb_layout_generation(d.get());
    int32_t complete = 0;
    REQUIRE(eb_layout_step(d.get(), 0, &complete) == EB_OK);
    CHECK(complete == 1);
    for (size_t c = 0; c < kChapters; ++c) CHECK(page_of(d.get(), chapter_mark(c)) == truth.start[c]);
    CHECK(eb_layout_generation(d.get()) == gen);
}

TEST_CASE("the estimate for chapters not laid out yet never pushes the book over the page limit")
{
    const std::string epub = ebt::multi_chapter_epub({kChapters});
    int32_t total = 0;
    size_t longest = 0;
    {
        const Lib lib;
        const Truth truth(epub);
        total = truth.total;
        for (size_t c = 0; c < kChapters; ++c) longest = truth.pages_in(c) > truth.pages_in(longest) ? c : longest;
        REQUIRE(truth.pages_in(longest) * static_cast<int32_t>(kChapters) > total + 20);
    }
    eb_limits limits{};
    limits.max_pages = static_cast<uint32_t>(total) + 5;
    const Lib lib(&limits);
    const DocPtr d = open_epub(epub);
    REQUIRE(eb_layout_lazy(d.get(), kW, kH, kEm, nullptr) == EB_OK);
    (void)page_of(d.get(), chapter_mark(longest));
    CHECK(count_of(d.get()) <= total + 5);
    run_steps(d.get(), chapter_mark(longest));
    CHECK(count_of(d.get()) == total);
}
