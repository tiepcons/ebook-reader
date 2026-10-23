#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <sqlite3.h>

TEST_CASE("sqlite in-memory select 1")
{
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open(":memory:", &db) == SQLITE_OK);
    sqlite3_stmt* st = nullptr;
    REQUIRE(sqlite3_prepare_v2(db, "select 1", -1, &st, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(st) == SQLITE_ROW);
    CHECK(sqlite3_column_int(st, 0) == 1);
    sqlite3_finalize(st);
    sqlite3_close(db);
}
