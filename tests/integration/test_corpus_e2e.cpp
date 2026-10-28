#include "eb_test_util.h"
#include "base/Text.h"
#include "lib/Metadata.h"
#include "lib/ProgressStore.h"
#include "lib/Schema.h"
#include "../unit/test_base_tmp.h"

#include <map>

namespace {

struct Row {
    std::string id;
    fs::path path;
    std::string fmt;
};

std::vector<Row> indexed_books(eb::Db& db)
{
    std::vector<Row> rows;
    auto st = db.prepare("SELECT id, path, fmt FROM books WHERE status = 0 ORDER BY path");
    REQUIRE(st.has_value());
    while (st->step().value() == eb::Step::Row) {
        rows.push_back({std::string(st->column_text(0)), fs::path(eb::utf8_to_wide(st->column_text(1)).value()), std::string(st->column_text(2))});
    }
    return rows;
}

void check_render_cover_text(eb_doc* d, int32_t pages, bool expectText)
{
    const eb_render_opts plain{1.0f, 0, 0xFFFFFF, 0};
    const BitmapPtr page = render0(d, plain);
    CHECK(not_uniform(*page));

    eb_bitmap* c = nullptr;
    REQUIRE(eb_render_cover(d, 128, &c) == EB_OK);
    const BitmapPtr cover(c);
    REQUIRE(cover != nullptr);
    CHECK(cover->width > 0);
    CHECK(cover->height > 0);

    bool anyText = false;
    for (int32_t p = 0; p < pages && !anyText; ++p) {
        eb_text* t = nullptr;
        if (eb_page_text(d, p, &t) != EB_OK) continue;
        const TextPtr guard(t);
        anyText = t != nullptr && t->char_count > 0;
    }
    if (expectText) CHECK(anyText);
}

} // namespace

TEST_CASE("corpus end-to-end: scan, open, render, cover, text, save and restore position")
{
    const Lib lib;
    const ebtest::TempDir data;
    auto opened = eb::Db::open(data.path() / L"lib.db");
    REQUIRE(opened.has_value());
    eb::Db db = std::move(*opened);
    REQUIRE(eb::migrate(db).has_value());

    eb::CrashLoopGuard guard{data.path() / L"marker.txt"};
    const std::atomic<bool> cancel{false};
    const fs::path roots[] = {fs::path(EB_CORPUS_DIR) / "good"};
    const auto stats = eb::scan_library(db, roots, eb::read_book_meta, guard, cancel);
    REQUIRE(stats.has_value());
    CHECK(stats->bad == 0);

    const auto rows = indexed_books(db);
    REQUIRE(rows.size() == corpus("good").size());

    eb::ProgressStore store(db);
    std::map<std::string, int> perFormat;
    for (const auto& row : rows) {
        INFO(row.path.string());
        ++perFormat[row.fmt];
        int32_t pages = 0;
        uint64_t mark = 0;
        {
            auto book = eb::open_book(row.path);
            REQUIRE(book.has_value());
            eb_doc* d = book->get();
            REQUIRE(eb_page_count(d, &pages) == EB_OK);
            REQUIRE(pages > 0);
            check_render_cover_text(d, pages, !has_image_ext(row.path));
            REQUIRE(eb_bookmark_of_page(d, pages - 1, &mark) == EB_OK);
        }
        REQUIRE(store.save(row.id, {mark, 1.0, -1, 0}).has_value());

        auto saved = store.load(row.id);
        REQUIRE(saved.has_value());
        REQUIRE(saved->has_value());
        auto reopened = eb::open_book(row.path);
        REQUIRE(reopened.has_value());
        int32_t restored = -1;
        REQUIRE(eb_page_of_bookmark(reopened->get(), (*saved)->mark, &restored) == EB_OK);
        CHECK(restored == pages - 1);
    }
    CHECK(perFormat.size() >= 6);
}
