#include "eb_test_util.h"

#include <cmath>
#include <string>

namespace {

struct Opened {
    DocPtr doc;
    int32_t pages = 0;
};

Opened open_laid_out(const fs::path& p, float em)
{
    Opened o;
    o.doc = open_doc(p);
    REQUIRE(eb_layout(o.doc.get(), 400, 600, em, nullptr) == EB_OK);
    REQUIRE(eb_page_count(o.doc.get(), &o.pages) == EB_OK);
    REQUIRE(o.pages > 0);
    return o;
}

std::vector<fs::path> samples()
{
    std::vector<fs::path> out;
    for (const char* ext : {".epub", ".fb2", ".azw3", ".txt", ".pdf", ".cbz"}) {
        const fs::path p = first_with_ext(ext);
        REQUIRE_FALSE(p.empty());
        out.push_back(p);
    }
    return out;
}

} // namespace

TEST_CASE("eb_bookmark survives close and reopen exactly")
{
    const Lib lib;
    for (const auto& p : samples()) {
        INFO(p.string());
        eb_bookmark bm = 0;
        int32_t k = 0;
        {
            const Opened o = open_laid_out(p, 12);
            for (const int32_t cand : {o.pages / 2, o.pages - 1, 0}) {
                k = cand;
                REQUIRE(eb_bookmark_of_page(o.doc.get(), k, &bm) == EB_OK);
                CHECK((bm >> 63) == 0);
                const Opened again = open_laid_out(p, 12);
                int32_t back = -1;
                eb_bookmark bm2 = 0;
                REQUIRE(eb_page_of_bookmark(again.doc.get(), bm, &back) == EB_OK);
                CHECK(back == k);
                REQUIRE(eb_bookmark_of_page(again.doc.get(), k, &bm2) == EB_OK);
                CHECK(bm2 == bm);
            }
        }
    }
}

TEST_CASE("eb_bookmark stays near the same place after relayout")
{
    const Lib lib;
    for (const auto& p : samples()) {
        INFO(p.string());
        const Opened small = open_laid_out(p, 12);
        const int32_t k = small.pages / 2;
        eb_bookmark bm = 0;
        REQUIRE(eb_bookmark_of_page(small.doc.get(), k, &bm) == EB_OK);
        REQUIRE(eb_layout(small.doc.get(), 400, 600, 18, nullptr) == EB_OK);
        int32_t pages18 = 0;
        int32_t page = -1;
        REQUIRE(eb_page_count(small.doc.get(), &pages18) == EB_OK);
        REQUIRE(eb_page_of_bookmark(small.doc.get(), bm, &page) == EB_OK);
        CHECK(page >= 0);
        CHECK(page < pages18);
        if (!eb_is_reflowable(small.doc.get())) {
            CHECK(page == k);
            continue;
        }
        const double expected = std::floor(static_cast<double>(k) * pages18 / small.pages + 0.5);
        const double slack = 2.0 + 0.02 * pages18;
        CHECK(std::fabs(page - expected) <= slack);
    }
}

TEST_CASE("eb_bookmark values grow strictly with reading order")
{
    const Lib lib;
    for (const auto& p : samples()) {
        INFO(p.string());
        const Opened o = open_laid_out(p, 12);
        eb_bookmark prev = 0;
        for (int32_t page = 0; page < o.pages; ++page) {
            eb_bookmark bm = 0;
            REQUIRE(eb_bookmark_of_page(o.doc.get(), page, &bm) == EB_OK);
            REQUIRE((bm >> 63) == 0);
            if (page > 0) REQUIRE(bm > prev);
            prev = bm;
        }
    }
}

TEST_CASE("eb_bookmark TOC targets decode to valid pages after reopen")
{
    const Lib lib;
    for (const auto& p : samples()) {
        INFO(p.string());
        std::vector<eb_bookmark> targets;
        std::vector<int32_t> pages;
        {
            const Opened o = open_laid_out(p, 12);
            eb_toc_item* items = nullptr;
            int32_t n = 0;
            REQUIRE(eb_toc(o.doc.get(), &items, &n) == EB_OK);
            for (int32_t i = 0; i < n; ++i) {
                if (items[i].page < 0) continue;
                targets.push_back(items[i].target);
                pages.push_back(items[i].page);
                CHECK((items[i].target >> 63) == 0);
            }
            eb_toc_free(items, n);
        }
        const Opened again = open_laid_out(p, 12);
        for (size_t i = 0; i < targets.size(); ++i) {
            int32_t page = -1;
            REQUIRE(eb_page_of_bookmark(again.doc.get(), targets[i], &page) == EB_OK);
            CHECK(page == pages[i]);
        }
    }
}
