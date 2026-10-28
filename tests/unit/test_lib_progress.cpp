#include <doctest/doctest.h>

#include "lib/Db.h"
#include "lib/ProgressStore.h"
#include "lib/Schema.h"
#include "test_base_tmp.h"

namespace {

eb::Db fresh(const ebtest::TempDir& t)
{
    auto db = eb::Db::open(t.path() / L"lib.db");
    REQUIRE(db.has_value());
    REQUIRE(eb::migrate(*db).has_value());
    return std::move(*db);
}

void add_book(eb::Db& db, std::string_view id, int status = 0)
{
    auto st = db.prepare("INSERT INTO books(id,path,folder,fmt,size,mtime,added_at,status) VALUES(?1,?1,'f','epub',1,1,1,?2)");
    REQUIRE(st.has_value());
    REQUIRE(st->bind_text(1, id).has_value());
    REQUIRE(st->bind_int(2, status).has_value());
    REQUIRE(st->step().value() == eb::Step::Done);
}

int64_t scalar(eb::Db& db, std::string_view sql)
{
    auto st = db.prepare(sql);
    REQUIRE(st.has_value());
    REQUIRE(st->step().value() == eb::Step::Row);
    return st->column_int(0);
}

}

TEST_CASE("progress save and load round-trip with UPSERT and last_opened")
{
    const ebtest::TempDir t;
    auto db = fresh(t);
    add_book(db, "a");
    eb::ProgressStore store(db);
    CHECK_FALSE(store.load("a").value().has_value());

    REQUIRE(store.save("a", {0xFFFFFFFF00000001ULL, 0.25, 7, 100}).has_value());
    auto p = store.load("a").value();
    REQUIRE(p.has_value());
    CHECK(p.value().mark == 0xFFFFFFFF00000001ULL);
    CHECK(p.value().percent == doctest::Approx(0.25));
    CHECK(p.value().tts_char == 7);
    CHECK(p.value().updated_at == 100);
    CHECK(scalar(db, "SELECT last_opened FROM books WHERE id='a'") == 100);

    REQUIRE(store.save("a", {5, 0.5, -1, 200}).has_value());
    CHECK(scalar(db, "SELECT count(*) FROM progress") == 1);
    CHECK(store.load("a").value().value().mark == 5);
    CHECK(scalar(db, "SELECT last_opened FROM books WHERE id='a'") == 200);
}

TEST_CASE("progress rejects unknown books and bad input")
{
    const ebtest::TempDir t;
    auto db = fresh(t);
    eb::ProgressStore store(db);
    CHECK_FALSE(store.save("ghost", {1, 0.1, -1, 1}).has_value());
    add_book(db, "a");
    CHECK_FALSE(store.save("a", {1, std::nan(""), -1, 1}).has_value());
    CHECK_FALSE(store.save("", {1, 0.1, -1, 1}).has_value());
}

TEST_CASE("progress saver coalesces updates and flush writes immediately")
{
    const ebtest::TempDir t;
    auto db = fresh(t);
    add_book(db, "a");
    eb::ProgressStore store(db);
    eb::ProgressSaver saver(store, "a");
    for (int i = 0; i < 50; ++i) {
        saver.update({static_cast<uint64_t>(i), 0.5, -1, i});
        saver.tick(static_cast<int64_t>(i) * 18);
    }
    saver.tick(900);
    CHECK_FALSE(store.load("a").value().has_value());
    saver.tick(1000);
    auto p = store.load("a").value();
    REQUIRE(p.has_value());
    CHECK(p.value().mark == 49);

    saver.update({77, 0.9, -1, 2000});
    saver.tick(1100);
    CHECK(store.load("a").value().value().mark == 49);
    saver.flush();
    CHECK(store.load("a").value().value().mark == 77);
}

TEST_CASE("progress saver keeps a failed save pending and retries")
{
    const ebtest::TempDir t;
    auto db = fresh(t);
    eb::ProgressStore store(db);
    eb::ProgressSaver saver(store, "a");
    saver.update({3, 0.3, -1, 5});
    saver.flush();
    add_book(db, "a");
    CHECK_FALSE(store.load("a").value().has_value());
    saver.flush();
    auto p = store.load("a").value();
    REQUIRE(p.has_value());
    CHECK(p.value().mark == 3);
}

TEST_CASE("progress persists to a second connection without a clean close")
{
    const ebtest::TempDir t;
    auto db = fresh(t);
    add_book(db, "a");
    eb::ProgressStore store(db);
    REQUIRE(store.save("a", {9, 0.9, 4, 50}).has_value());
    auto other = eb::Db::open(t.path() / L"lib.db");
    REQUIRE(other.has_value());
    eb::ProgressStore store2(*other);
    CHECK(store2.load("a").value().value().mark == 9);
}

TEST_CASE("bookmark CRUD, ordering, sanitising and cascade")
{
    const ebtest::TempDir t;
    auto db = fresh(t);
    add_book(db, "a");
    add_book(db, "b");
    auto id2 = eb::add_bookmark(db, "a", 20, L"second");
    auto id1 = eb::add_bookmark(db, "a", 10, L"");
    auto id3 = eb::add_bookmark(db, "a", 30, std::wstring(500, L'x') + L"\n");
    REQUIRE((id1.has_value() && id2.has_value() && id3.has_value()));
    REQUIRE(eb::add_bookmark(db, "b", 1, L"other").has_value());
    REQUIRE(eb::add_bookmark(db, "a", 40, L"tab\there\x01").has_value());

    auto list = eb::list_bookmarks(db, "a").value();
    REQUIRE(list.size() == 4);
    CHECK(list[0].id == *id1);
    CHECK(list[0].label.empty());
    CHECK(list[1].label == L"second");
    CHECK(list[2].label.size() == eb::kMaxBookmarkLabel);
    CHECK(list[3].label == L"tabhere");

    REQUIRE(eb::remove_bookmark(db, *id2).has_value());
    CHECK(eb::list_bookmarks(db, "a").value().size() == 3);

    REQUIRE(db.exec("DELETE FROM books WHERE id='a'").has_value());
    CHECK(eb::list_bookmarks(db, "a").value().empty());
    CHECK(eb::list_bookmarks(db, "b").value().size() == 1);
}

TEST_CASE("bookmark rejects an unknown book")
{
    const ebtest::TempDir t;
    auto db = fresh(t);
    CHECK_FALSE(eb::add_bookmark(db, "ghost", 1, L"x").has_value());
}
