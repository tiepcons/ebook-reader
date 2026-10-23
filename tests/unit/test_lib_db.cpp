#include <doctest/doctest.h>

#include "lib/Db.h"
#include "lib/Schema.h"
#include "test_base_tmp.h"

namespace {

int64_t scalar(eb::Db& db, std::string_view sql)
{
    auto st = db.prepare(sql);
    REQUIRE(st.has_value());
    REQUIRE(st->step().value() == eb::Step::Row);
    return st->column_int(0);
}

eb::Db fresh(const ebtest::TempDir& t)
{
    auto db = eb::Db::open(t.path() / L"lib.db");
    REQUIRE(db.has_value());
    REQUIRE(eb::migrate(*db).has_value());
    return std::move(*db);
}

void add_book(eb::Db& db, std::string_view id, std::string_view title)
{
    auto st = db.prepare("INSERT INTO books(id,path,folder,title,fmt,size,mtime,added_at) VALUES(?1,?1,'f',?2,'epub',1,1,1)");
    REQUIRE(st.has_value());
    REQUIRE(st->bind_text(1, id).has_value());
    REQUIRE(st->bind_text(2, title).has_value());
    REQUIRE(st->step().value() == eb::Step::Done);
}

}

TEST_CASE("fresh database gets the v1 schema")
{
    const ebtest::TempDir t;
    auto db = fresh(t);
    CHECK(eb::user_version(db).value() == 1);
    CHECK(scalar(db, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN "
                     "('books','progress','bookmarks','tts_usage','kv')") == 5);
    CHECK(scalar(db, "SELECT count(*) FROM sqlite_master WHERE type='index' AND name LIKE 'books_%'") == 4);
}

TEST_CASE("migrate is idempotent and refuses a newer database")
{
    const ebtest::TempDir t;
    auto db = fresh(t);
    add_book(db, "a", "x");
    CHECK(eb::migrate(db).has_value());
    CHECK(scalar(db, "SELECT count(*) FROM books") == 1);
    REQUIRE(db.exec("PRAGMA user_version = 99").has_value());
    auto r = eb::migrate(db);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == eb::ErrorCode::Format);
}

TEST_CASE("connection settings are applied")
{
    const ebtest::TempDir t;
    auto db = fresh(t);
    auto st = db.prepare("PRAGMA journal_mode");
    REQUIRE(st->step().value() == eb::Step::Row);
    CHECK(st->column_text(0) == "wal");
    CHECK(scalar(db, "PRAGMA foreign_keys") == 1);
    CHECK(scalar(db, "PRAGMA synchronous") == 1);
    CHECK(scalar(db, "PRAGMA busy_timeout") == 2000);
}

TEST_CASE("transaction rolls back unless committed")
{
    const ebtest::TempDir t;
    auto db = fresh(t);
    {
        auto tx = eb::Tx::begin(db);
        REQUIRE(tx.has_value());
        add_book(db, "a", "x");
    }
    CHECK(scalar(db, "SELECT count(*) FROM books") == 0);
    {
        auto tx = eb::Tx::begin(db);
        add_book(db, "a", "x");
        REQUIRE(tx->commit().has_value());
    }
    CHECK(scalar(db, "SELECT count(*) FROM books") == 1);
}

TEST_CASE("deleting a book cascades to progress and bookmarks")
{
    const ebtest::TempDir t;
    auto db = fresh(t);
    add_book(db, "a", "x");
    REQUIRE(db.exec("INSERT INTO progress VALUES('a',5,0.5,-1,1)").has_value());
    REQUIRE(db.exec("INSERT INTO bookmarks(book_id,bookmark,created_at) VALUES('a',1,1)").has_value());
    REQUIRE(db.exec("DELETE FROM books WHERE id='a'").has_value());
    CHECK(scalar(db, "SELECT count(*) FROM progress") == 0);
    CHECK(scalar(db, "SELECT count(*) FROM bookmarks") == 0);
    CHECK(db.exec("INSERT INTO progress VALUES('zzz',5,0.5,-1,1)").error().code == eb::ErrorCode::Format);
}

TEST_CASE("UTF-8 text, null, real and errors round-trip")
{
    const ebtest::TempDir t;
    auto db = fresh(t);
    const std::string title = "Tiếng Việt: Đắc Nhân Tâm";
    add_book(db, "a", title);
    auto st = db.prepare("SELECT title, author, 2.5 FROM books WHERE id=?1");
    REQUIRE(st->bind_text(1, "a").has_value());
    REQUIRE(st->step().value() == eb::Step::Row);
    CHECK(st->column_text(0) == title);
    CHECK(st->is_null(1));
    CHECK(st->column_double(2) == 2.5);
    CHECK(st->step().value() == eb::Step::Done);
    CHECK_FALSE(db.prepare("SELEC nonsense").has_value());
    CHECK(eb::Db::open(t.path() / L"missing" / L"x.db").error().code == eb::ErrorCode::NotFound);
}

TEST_CASE("empty text binds an empty string, not NULL")
{
    const ebtest::TempDir t;
    auto db = fresh(t);
    auto st = db.prepare("SELECT typeof(?1)");
    REQUIRE(st->bind_text(1, std::string_view{}).has_value());
    REQUIRE(st->step().value() == eb::Step::Row);
    CHECK(st->column_text(0) == "text");
}

TEST_CASE("prepare rejects empty statements and trailing text")
{
    const ebtest::TempDir t;
    auto db = fresh(t);
    CHECK_FALSE(db.prepare("").has_value());
    CHECK_FALSE(db.prepare("-- only a comment").has_value());
    CHECK_FALSE(db.prepare("SELECT 1; SELECT 2").has_value());
    CHECK(db.prepare("SELECT 1; \n").has_value());
}
