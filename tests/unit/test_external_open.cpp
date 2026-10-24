#include <doctest/doctest.h>

#include "lib/LibraryModel.h"
#include "lib/Schema.h"
#include "test_base_tmp.h"

#include <fstream>
#include <string>

namespace {

struct ExtFixture {
    ebtest::TempDir dir;
    eb::Db db = open();

    eb::Db open()
    {
        auto d = eb::Db::open(dir.path() / L"lib.db");
        REQUIRE(d.has_value());
        REQUIRE(eb::migrate(*d).has_value());
        return std::move(*d);
    }

    std::filesystem::path make(const std::wstring& name, const std::string& body)
    {
        const auto p = dir.path() / name;
        std::ofstream(p, std::ios::binary) << body;
        return p;
    }

    std::string text(const std::string& id, const char* col)
    {
        auto st = db.prepare(std::string("SELECT ") + col + " FROM books WHERE id=?1");
        REQUIRE(st.has_value());
        REQUIRE(st->bind_text(1, id).has_value());
        REQUIRE(st->step().has_value());
        return std::string(st->column_text(0));
    }

    int rows()
    {
        auto st = db.prepare("SELECT COUNT(*) FROM books");
        REQUIRE(st.has_value());
        REQUIRE(st->step().has_value());
        return static_cast<int>(st->column_int(0));
    }
};

eb::BookMeta meta(std::wstring title)
{
    eb::BookMeta m;
    m.title = std::move(title);
    m.author = L"Frank Herbert";
    m.fmt = L"txt";
    return m;
}

}

TEST_CASE_FIXTURE(ExtFixture, "resolve_external_book accepts only existing supported files")
{
    const auto good = make(L"a.txt", "hello");
    const auto r = eb::resolve_external_book(good.wstring());
    REQUIRE(r.has_value());
    CHECK(r->is_absolute());
    CHECK_FALSE(eb::resolve_external_book(make(L"a.exe", "x").wstring()).has_value());
    CHECK_FALSE(eb::resolve_external_book((dir.path() / L"nope.txt").wstring()).has_value());
    CHECK_FALSE(eb::resolve_external_book(dir.path().wstring()).has_value());
    CHECK_FALSE(eb::resolve_external_book(L"").has_value());
    CHECK_FALSE(eb::resolve_external_book(std::wstring(40000, L'a')).has_value());
}

TEST_CASE_FIXTURE(ExtFixture, "upsert_external inserts an openable row and is idempotent")
{
    const auto file = make(L"Dune.txt", "arrakis");
    const auto id = eb::upsert_external(db, file, meta(L"Dune"));
    REQUIRE(id.has_value());
    CHECK(rows() == 1);
    CHECK(text(*id, "title") == "Dune");
    CHECK(text(*id, "author") == "Frank Herbert");
    CHECK(text(*id, "status") == "0");
    CHECK(text(*id, "folder") == dir.path().string());
    const auto again = eb::upsert_external(db, file, meta(L"Other"));
    REQUIRE(again.has_value());
    CHECK(*again == *id);
    CHECK(rows() == 1);
}

TEST_CASE_FIXTURE(ExtFixture, "find_book_by_path matches case-insensitively and skips hidden rows")
{
    const auto file = make(L"Dune.txt", "arrakis");
    const auto id = eb::upsert_external(db, file, meta(L"Dune"));
    REQUIRE(id.has_value());
    CHECK(eb::find_book_by_path(db, file).value_or("") == *id);
    std::wstring upper = file.wstring();
    for (auto& c : upper)
        c = static_cast<wchar_t>(towupper(c));
    CHECK(eb::find_book_by_path(db, upper).value_or("") == *id);
    REQUIRE(eb::hide_book(db, *id).has_value());
    CHECK_FALSE(eb::find_book_by_path(db, file).has_value());
    const auto revived = eb::upsert_external(db, file, meta(L"Dune"));
    REQUIRE(revived.has_value());
    CHECK(*revived == *id);
    CHECK(text(*id, "status") == "0");
    CHECK(eb::find_book_by_path(db, file).has_value());
}

TEST_CASE_FIXTURE(ExtFixture, "upsert_external reuses the row of a moved copy with the same content")
{
    const auto first = make(L"One.txt", "same bytes");
    const auto id = eb::upsert_external(db, first, meta(L"One"));
    REQUIRE(id.has_value());
    const auto moved = make(L"Two.txt", "same bytes");
    const auto id2 = eb::upsert_external(db, moved, meta(L"Two"));
    REQUIRE(id2.has_value());
    CHECK(*id2 == *id);
    CHECK(rows() == 1);
    CHECK(text(*id, "path") == moved.string());
}
