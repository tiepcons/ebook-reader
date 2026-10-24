#include <doctest/doctest.h>

#include "lib/AzureUsage.h"
#include "lib/Schema.h"
#include "test_base_tmp.h"

#include <atomic>
#include <functional>
#include <thread>

namespace {

constexpr int64_t kOct31Last = 1793491199;  // 2026-10-31T23:59:59Z
constexpr int64_t kNov1 = 1793491200;       // 2026-11-01T00:00:00Z
constexpr int64_t kDec1 = 1796083200;       // 2026-12-01T00:00:00Z
constexpr int64_t kJan1 = 1798761600;       // 2027-01-01T00:00:00Z

eb::Db fresh(const ebtest::TempDir& t)
{
    auto db = eb::Db::open(t.path() / L"lib.db");
    REQUIRE(db.has_value());
    REQUIRE(eb::migrate(*db).has_value());
    return std::move(*db);
}

}

// Azure Speech pricing: characters incl. spaces/punctuation (SSML tags are not billed), CJK counted twice.
// A character is a Unicode code point, so each combining mark of a decomposed (NFD) text counts as one.
TEST_CASE("billable_chars counts code points with CJK doubled")
{
    CHECK(eb::billable_chars("") == 0);
    CHECK(eb::billable_chars("Hello, world!") == 13);
    CHECK(eb::billable_chars("Vi\xE1\xBB\x87t") == 4);
    CHECK(eb::billable_chars("Vie\xCC\xA3\xCC\x82t") == 6);
    CHECK(eb::billable_chars("\xE6\xBC\xA2\xE5\xAD\x97") == 4);
    CHECK(eb::billable_chars("\xE3\x81\x82") == 2);
    CHECK(eb::billable_chars("\xED\x95\x9C") == 2);
    CHECK(eb::billable_chars("\xF0\x9F\x98\x80") == 1);
    CHECK(eb::billable_chars("\xFF\xC0" "a") == 3);
    CHECK(eb::billable_chars("\xE3\x81") == 2);
    CHECK(eb::billable_chars("\xC0\x80") == 2);
}

TEST_CASE("month_utc and rollover")
{
    CHECK(eb::month_utc(kOct31Last) == "2026-10");
    CHECK(eb::month_utc(kNov1) == "2026-11");
    CHECK(eb::month_utc(kJan1 - 1) == "2026-12");
    CHECK(eb::month_utc(0) == "1970-01");
    CHECK(eb::month_utc(-1) == "1969-12");
}

TEST_CASE("add accumulates per month")
{
    const ebtest::TempDir t;
    auto db = fresh(t);
    eb::AzureUsage u(db);
    REQUIRE(u.add(kOct31Last, 100).has_value());
    REQUIRE(u.add(kOct31Last, 50).has_value());
    CHECK(u.status(kOct31Last)->used == 150);
    CHECK(u.status(kNov1)->used == 0);
    REQUIRE(u.add(kNov1, 7).has_value());
    CHECK(u.status(kNov1)->used == 7);
    CHECK(u.status(kOct31Last)->used == 150);
}

TEST_CASE("status math and reset date")
{
    const ebtest::TempDir t;
    auto db = fresh(t);
    eb::AzureUsage u(db);
    REQUIRE(u.calibrate(kOct31Last, 123456).has_value());
    auto s = u.status(kOct31Last).value();
    CHECK(s.quota == 500000);
    CHECK(s.percent_used == doctest::Approx(24.7));
    CHECK(s.percent_left == doctest::Approx(75.3));
    CHECK(s.reset_utc_unix == kNov1);
    CHECK_FALSE(s.over_quota);
    CHECK(u.status(kDec1 + 5)->reset_utc_unix == kJan1);

    REQUIRE(u.calibrate(kOct31Last, 600000).has_value());
    s = u.status(kOct31Last).value();
    CHECK(s.over_quota);
    CHECK(s.percent_used == doctest::Approx(100.0));
    CHECK(s.percent_left == doctest::Approx(0.0));

    auto kv = db.prepare("INSERT INTO kv(key,value) VALUES('azure.quota','5000')");
    REQUIRE(kv.has_value());
    REQUIRE(kv->step().has_value());
    CHECK(u.status(kOct31Last)->quota == 5000);
}

TEST_CASE("reset_month zeroes the counter")
{
    const ebtest::TempDir t;
    auto db = fresh(t);
    eb::AzureUsage u(db);
    REQUIRE(u.add(kNov1, 450000).has_value());
    CHECK(u.check_warning(kNov1).value() == eb::Warn::P80);
    REQUIRE(u.reset_month(kNov1).has_value());
    CHECK(u.status(kNov1)->used == 0);
    REQUIRE(u.add(kNov1, 450000).has_value());
    CHECK(u.check_warning(kNov1).value() == eb::Warn::P80);
}

TEST_CASE("warnings fire once per level per month")
{
    const ebtest::TempDir t;
    auto db = fresh(t);
    eb::AzureUsage u(db);
    CHECK(u.check_warning(kOct31Last).value() == eb::Warn::None);
    REQUIRE(u.add(kOct31Last, 400000).has_value());
    CHECK(u.check_warning(kOct31Last).value() == eb::Warn::P80);
    CHECK(u.check_warning(kOct31Last).value() == eb::Warn::None);
    REQUIRE(u.add(kOct31Last, 80000).has_value());
    CHECK(u.check_warning(kOct31Last).value() == eb::Warn::P95);
    REQUIRE(u.add(kOct31Last, 20000).has_value());
    CHECK(u.check_warning(kOct31Last).value() == eb::Warn::P100);
    CHECK(u.check_warning(kOct31Last).value() == eb::Warn::None);

    REQUIRE(u.add(kNov1, 400000).has_value());
    CHECK(u.check_warning(kNov1).value() == eb::Warn::P80);
    REQUIRE(u.add(kDec1, 500000).has_value());
    CHECK(u.check_warning(kDec1).value() == eb::Warn::P100);
}

TEST_CASE("format_status_vi renders Vietnamese grouping")
{
    const ebtest::TempDir t;
    auto db = fresh(t);
    eb::AzureUsage u(db);
    REQUIRE(u.calibrate(kOct31Last, 123456).has_value());
    CHECK(eb::format_status_vi(u.status(kOct31Last).value())
        == L"Đã dùng 123.456 / 500.000 ký tự (24,7%) · Còn 75,3% · Reset 01/11 (UTC)");
    REQUIRE(u.calibrate(kOct31Last, 0).has_value());
    CHECK(eb::format_status_vi(u.status(kOct31Last).value())
        == L"Đã dùng 0 / 500.000 ký tự (0,0%) · Còn 100,0% · Reset 01/11 (UTC)");
    CHECK(eb::format_status_vi(u.status(kDec1).value()).ends_with(L"Reset 01/01 (UTC)"));
}

TEST_CASE("concurrent add from two connections sums correctly")
{
    const ebtest::TempDir t;
    {
        auto first = fresh(t);
    }
    std::atomic<int> failures{0};
    auto db1 = eb::Db::open(t.path() / L"lib.db", 10000);
    auto db2 = eb::Db::open(t.path() / L"lib.db", 10000);
    REQUIRE(db1.has_value());
    REQUIRE(db2.has_value());
    const auto worker = [&](eb::Db& db) {
        eb::AzureUsage u(db);
        for (int i = 0; i < 200; ++i) {
            if (!u.add(kNov1, 3))
                ++failures;
        }
    };
    std::thread a(worker, std::ref(*db1));
    std::thread b(worker, std::ref(*db2));
    a.join();
    b.join();
    CHECK(failures.load() == 0);
    auto db = fresh(t);
    CHECK(eb::AzureUsage(db).status(kNov1)->used == 1200);
}
