#include <doctest/doctest.h>

#include "base/Text.h"
#include "lib/LibraryModel.h"
#include "lib/Schema.h"
#include "test_base_tmp.h"
#include "test_perf.h"

#include <chrono>
#include <string>

namespace {

struct ModelFixture {
    ebtest::TempDir dir;
    eb::Db db = open();
    int n = 0;

    eb::Db open()
    {
        auto d = eb::Db::open(dir.path() / L"lib.db");
        REQUIRE(d.has_value());
        REQUIRE(eb::migrate(*d).has_value());
        return std::move(*d);
    }

    void add(const std::string& title, const std::string& title_nd, const std::string& author, const std::string& folder,
        const std::string& fmt = "epub", int status = 0, int64_t added = 0, double percent = -1)
    {
        const std::string id = "id" + std::to_string(++n);
        auto st = db.prepare("INSERT INTO books(id,path,folder,title,author,title_nd,author_nd,fmt,size,mtime,added_at,status)"
                             " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,1,1,?9,?10)");
        REQUIRE(st.has_value());
        REQUIRE(st->bind_text(1, id).has_value());
        REQUIRE(st->bind_text(2, folder + "\\" + id + "." + fmt).has_value());
        REQUIRE(st->bind_text(3, folder).has_value());
        REQUIRE(st->bind_text(4, title).has_value());
        REQUIRE(st->bind_text(5, author).has_value());
        REQUIRE(st->bind_text(6, title_nd).has_value());
        REQUIRE(st->bind_text(7, author).has_value());
        REQUIRE(st->bind_text(8, fmt).has_value());
        REQUIRE(st->bind_int(9, added).has_value());
        REQUIRE(st->bind_int(10, status).has_value());
        REQUIRE(st->step().has_value());
        if (percent >= 0) {
            auto p = db.prepare("INSERT INTO progress(book_id,bookmark,percent,updated_at) VALUES(?1,0,?2,1)");
            REQUIRE(p.has_value());
            REQUIRE(p->bind_text(1, id).has_value());
            REQUIRE(p->bind_double(2, percent).has_value());
            REQUIRE(p->step().has_value());
        }
    }

    std::vector<std::wstring> titles(const eb::Query& q)
    {
        auto r = eb::query_books(db, q);
        REQUIRE(r.has_value());
        std::vector<std::wstring> out;
        for (const auto& b : *r)
            out.push_back(eb::utf8_to_wide(b.title).value());
        return out;
    }
};

using V = std::vector<std::wstring>;

}

TEST_CASE("model: vietnamese search ignores diacritics")
{
    ModelFixture f;
    f.add("Truyện Kiều", "truyen kieu", "nguyen du", "C:\\b");
    f.add("Dế Mèn", "de men", "to hoai", "C:\\b");
    eb::Query q;
    q.text = L"kieu";
    CHECK(f.titles(q) == V{L"Truyện Kiều"});
    q.text = L"KIỀU truyện";
    CHECK(f.titles(q) == V{L"Truyện Kiều"});
    q.text = L"hoai";
    CHECK(f.titles(q) == V{L"Dế Mèn"});
    q.text = L"zzz";
    CHECK(f.titles(q).empty());
}

TEST_CASE("model: LIKE wildcards in the search text are literal")
{
    ModelFixture f;
    f.add("100% Cotton", "100% cotton", "", "C:\\b");
    f.add("A_B", "a_b", "", "C:\\b");
    f.add("AxB", "axb", "", "C:\\b");
    f.add("Plain", "plain", "", "C:\\b");
    eb::Query q;
    q.text = L"%";
    CHECK(f.titles(q) == V{L"100% Cotton"});
    q.text = L"a_b";
    CHECK(f.titles(q) == V{L"A_B"});
    q.text = L"!";
    CHECK(f.titles(q).empty());
}

TEST_CASE("model: folder prefix matches the folder and its subfolders only")
{
    ModelFixture f;
    f.add("One", "one", "", "C:\\lib");
    f.add("Two", "two", "", "C:\\lib\\sub");
    f.add("Three", "three", "", "C:\\library");
    f.add("Four", "four", "", "C:\\lib_x");
    eb::Query q;
    q.folder = L"C:\\lib";
    CHECK(f.titles(q) == V{L"One", L"Two"});
    q.folder = L"C:\\lib\\sub";
    CHECK(f.titles(q) == V{L"Two"});
    q.folder = L"C:\\lib_";
    CHECK(f.titles(q).empty());
}

TEST_CASE("model: format filter and sorting")
{
    ModelFixture f;
    f.add("Beta", "beta", "zed", "C:\\b", "pdf", 0, 10);
    f.add("Alpha", "alpha", "yan", "C:\\b", "epub", 0, 30);
    f.add("Gamma", "gamma", "abe", "C:\\b", "epub", 0, 20);
    eb::Query q;
    CHECK(f.titles(q) == V{L"Alpha", L"Beta", L"Gamma"});
    q.sort = eb::SortBy::Author;
    CHECK(f.titles(q) == V{L"Gamma", L"Alpha", L"Beta"});
    q.sort = eb::SortBy::Added;
    CHECK(f.titles(q) == V{L"Alpha", L"Gamma", L"Beta"});
    REQUIRE(f.db.exec("UPDATE books SET last_opened=5 WHERE title='Beta'").has_value());
    REQUIRE(f.db.exec("UPDATE books SET last_opened=9 WHERE title='Gamma'").has_value());
    q.sort = eb::SortBy::LastOpened;
    CHECK(f.titles(q) == V{L"Gamma", L"Beta", L"Alpha"});
    q.fmt = "EPUB";
    CHECK(f.titles(q) == V{L"Gamma", L"Alpha"});
}

TEST_CASE("model: reading_only keeps 0 < percent < 0.99 and reports it")
{
    ModelFixture f;
    f.add("None", "none", "", "C:\\b");
    f.add("Zero", "zero", "", "C:\\b", "epub", 0, 0, 0.0);
    f.add("Half", "half", "", "C:\\b", "epub", 0, 0, 0.5);
    f.add("Done", "done", "", "C:\\b", "epub", 0, 0, 1.0);
    eb::Query q;
    q.reading_only = true;
    auto r = eb::query_books(f.db, q);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 1);
    CHECK((*r)[0].title == "Half");
    CHECK((*r)[0].percent == doctest::Approx(0.5));
    q.reading_only = false;
    r = eb::query_books(f.db, q);
    REQUIRE(r.has_value());
    CHECK(r->size() == 4);
}

TEST_CASE("model: missing books are returned flagged and excluded from folders")
{
    ModelFixture f;
    f.add("Here", "here", "", "C:\\a");
    f.add("Gone", "gone", "", "C:\\gone", "epub", 1);
    f.add("Bad", "bad", "", "C:\\a", "epub", 2);
    auto r = eb::query_books(f.db, {});
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 3);
    CHECK((*r)[1].title == "Gone");
    CHECK((*r)[1].status == 1);
    CHECK((*r)[0].status == 2);
    auto folders = eb::list_folders(f.db);
    REQUIRE(folders.has_value());
    CHECK(*folders == V{L"C:\\a"});
}

TEST_CASE("model: book_path strips the marker; hidden books vanish until unhidden")
{
    ModelFixture f;
    f.add("Gone", "gone", "", "C:\\x", "epub", 1, 0, 0.3);
    REQUIRE(f.db.exec("UPDATE books SET path=path||char(31)||'abcdef'").has_value());
    auto p = eb::book_path(f.db, "id1");
    REQUIRE(p.has_value());
    CHECK(*p == L"C:\\x\\id1.epub");
    CHECK(eb::book_path(f.db, "nope").error().code == eb::ErrorCode::NotFound);
    f.add("Here", "here", "", "C:\\y");
    REQUIRE(f.db.exec("UPDATE books SET status=0 WHERE id='id1'").has_value());
    REQUIRE(eb::hide_book(f.db, "id1").has_value());
    CHECK(f.titles({}) == V{L"Here"});
    CHECK(eb::list_folders(f.db).value() == V{L"C:\\y"});
    REQUIRE(eb::unhide_all_books(f.db).has_value());
    CHECK(f.titles({}) == V{L"Gone", L"Here"});
}

TEST_CASE("model: folder tree is relative to each root")
{
    const V folders = {L"D:\\Books", L"D:\\Books\\Sci\\Phys", L"D:\\books\\Art", L"E:\\Elsewhere", L"D:\\Booksy"};
    const V roots = {L"D:\\Books\\"};
    const auto t = eb::build_folder_tree(folders, roots);
    REQUIRE(t.size() == 6);
    CHECK(t[0].label == L"Books");
    CHECK(t[0].parent == -1);
    CHECK(t[1].label == L"Art");
    CHECK(t[1].parent == 0);
    CHECK(t[2].label == L"Sci");
    CHECK(t[3].label == L"Phys");
    CHECK(t[3].prefix == L"D:\\Books\\Sci\\Phys");
    CHECK(t[3].parent == 2);
    CHECK(t[4].label == L"D:\\Booksy");
    CHECK(t[5].label == L"E:\\Elsewhere");
}

TEST_CASE("model: 50000 rows query within budget")
{
    ModelFixture f;
    REQUIRE(f.db.exec("WITH RECURSIVE c(i) AS (SELECT 0 UNION ALL SELECT i+1 FROM c WHERE i<49999) "
                      "INSERT INTO books(id,path,folder,title,author,title_nd,author_nd,fmt,size,mtime,added_at,status) "
                      "SELECT 'id'||i,'C:\\lib\\id'||i||'.epub','C:\\lib\\f'||(i%20),'Sach so '||i,'Tac gia '||(i%50),"
                      "'sach so '||i,'tac gia '||(i%50),'epub',1,1,i,0 FROM c")
                .has_value());
    const auto timed = [&](const eb::Query& q, size_t min_rows) {
        const auto t0 = std::chrono::steady_clock::now();
        auto r = eb::query_books(f.db, q);
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        REQUIRE(r.has_value());
        CHECK(r->size() >= min_rows);
        return ms;
    };
    const double all_ms = timed({}, 50000);
    eb::Query q;
    q.text = L"so 49";
    const double search_ms = timed(q, 1);
#ifdef NDEBUG
    CHECK(all_ms < ebtest::perf_budget(60.0));
    CHECK(search_ms < ebtest::perf_budget(120.0));
#else
    CHECK(all_ms >= 0.0);
    CHECK(search_ms >= 0.0);
#endif
}

TEST_CASE("model: 5000 rows query within budget")
{
    ModelFixture f;
    REQUIRE(f.db.exec("BEGIN").has_value());
    size_t expected = 0;
    for (int i = 0; i < 5000; ++i) {
        expected += std::to_string(i).contains("49") || i % 50 == 49 ? 1 : 0;
        f.add("Sach so " + std::to_string(i), "sach so " + std::to_string(i), "tac gia " + std::to_string(i % 50),
            "C:\\lib\\f" + std::to_string(i % 20));
    }
    REQUIRE(f.db.exec("COMMIT").has_value());
    eb::Query q;
    q.text = L"so 49";
    q.folder = L"C:\\lib";
    const auto t0 = std::chrono::steady_clock::now();
    auto r = eb::query_books(f.db, q);
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(r.has_value());
    CHECK(r->size() == expected);
#ifdef NDEBUG
    CHECK(ms < ebtest::perf_budget(30.0));
#else
    CHECK(ms < ebtest::perf_budget(500.0));
#endif
    const auto all = eb::query_books(f.db, {});
    REQUIRE(all.has_value());
    CHECK(all->size() == 5000);
}
