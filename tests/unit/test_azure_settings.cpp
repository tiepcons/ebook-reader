#include <doctest/doctest.h>

#include <windows.h>

#include "tts/azure/AzureProbe.h"
#include "ui/AzureSettings.h"

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>

namespace {

class StatusHttp final : public eb::HttpClient {
public:
    explicit StatusHttp(int status) : status_(status) {}
    eb::HttpResponse send(const eb::HttpRequest& r) noexcept override
    {
        host = r.host;
        path = r.path;
        key = r.key != nullptr ? std::wstring(r.key->view()) : std::wstring{};
        return {status_, {}, 0};
    }
    void cancel() noexcept override {}
    void shutdown() noexcept override {}
    std::wstring host, path, key;

private:
    int status_;
};

}

TEST_CASE("parse_region trims, lowercases and validates")
{
    CHECK(eb::parse_region(L"  SoutheastAsia ") == std::optional<std::string>("southeastasia"));
    CHECK(eb::parse_region(L"eastus2") == std::optional<std::string>("eastus2"));
    CHECK(eb::parse_region(L"") == std::optional<std::string>(""));
    CHECK_FALSE(eb::parse_region(L"ab").has_value());
    CHECK_FALSE(eb::parse_region(L"east-us").has_value());
    CHECK_FALSE(eb::parse_region(L"a.b.c").has_value());
    CHECK_FALSE(eb::parse_region(L"éastus").has_value());
    CHECK_FALSE(eb::parse_region(std::wstring(33, L'a')).has_value());
}

TEST_CASE("parse_char_count accepts group separators and enforces the range")
{
    using eb::parse_char_count;
    CHECK(parse_char_count(L"500000", 1, eb::kMaxAzureQuota) == std::optional<uint64_t>(500000));
    CHECK(parse_char_count(L"500.000", 1, eb::kMaxAzureQuota) == std::optional<uint64_t>(500000));
    CHECK(parse_char_count(L" 1,000 ", 1, eb::kMaxAzureQuota) == std::optional<uint64_t>(1000));
    CHECK(parse_char_count(L"0", 0, 10) == std::optional<uint64_t>(0));
    CHECK_FALSE(parse_char_count(L"0", 1, 10).has_value());
    CHECK_FALSE(parse_char_count(L"11", 0, 10).has_value());
    CHECK_FALSE(parse_char_count(L"", 0, 10).has_value());
    CHECK_FALSE(parse_char_count(L"12a", 0, 100).has_value());
    CHECK_FALSE(parse_char_count(L"-5", 0, 100).has_value());
    CHECK_FALSE(parse_char_count(L"99999999999999999999", 0, UINT64_MAX).has_value());
}

TEST_CASE("tier parsing defaults to F0")
{
    CHECK(eb::parse_tier("S0") == eb::AzureTier::S0);
    CHECK(eb::parse_tier("F0") == eb::AzureTier::F0);
    CHECK(eb::parse_tier("junk") == eb::AzureTier::F0);
    CHECK(std::string(eb::tier_name(eb::AzureTier::S0)) == "S0");
}

TEST_CASE("quota percentage text rounds down and is clamped")
{
    eb::UsageStatus s;
    s.percent_left = 75.0;
    CHECK(eb::bar_quota_text(s) == L"Azure 75%");
    CHECK(eb::tray_quota_text(s) == L"Azure còn 75%");
    s.percent_left = 99.9;
    CHECK(eb::percent_left_whole(s) == 99);
    s.percent_left = 0;
    CHECK(eb::bar_quota_text(s) == L"Azure 0%");
    s.percent_left = -3;
    CHECK(eb::percent_left_whole(s) == 0);
    s.percent_left = 250;
    CHECK(eb::percent_left_whole(s) == 100);
}

TEST_CASE("warning and probe messages")
{
    CHECK(eb::warn_text(eb::Warn::None).empty());
    CHECK_FALSE(eb::warn_text(eb::Warn::P80).empty());
    CHECK(eb::warn_text(eb::Warn::P80) != eb::warn_text(eb::Warn::P95));
    CHECK_FALSE(eb::warn_text(eb::Warn::P100).empty());
    CHECK(eb::probe_text(401).find(L"401") != std::wstring::npos);
    CHECK(eb::probe_text(403).find(L"403") != std::wstring::npos);
    CHECK(eb::probe_text(401).find(L"khóa") != std::wstring::npos);
    CHECK(eb::probe_text(200) != eb::probe_text(0));
    CHECK(eb::probe_text(500).find(L"500") != std::wstring::npos);
}

TEST_CASE("AzureProbe reports the status of GET voices/list asynchronously")
{
    auto http = std::make_unique<StatusHttp>(401);
    StatusHttp* raw = http.get();
    eb::AzureProbe probe(std::move(http));
    auto key = eb::SecretString::copy_of(L"SECRETKEY0123456789abcdefghijklmn");
    REQUIRE(key.has_value());
    std::promise<int> got;
    auto fut = got.get_future();
    REQUIRE(probe.start("southeastasia", std::move(*key), [&](int s) { got.set_value(s); }));
    REQUIRE(fut.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    CHECK(fut.get() == 401);
    CHECK(raw->host == L"southeastasia.tts.speech.microsoft.com");
    CHECK(raw->path == L"/cognitiveservices/voices/list");
    CHECK(raw->key == L"SECRETKEY0123456789abcdefghijklmn");
}

TEST_CASE("AzureProbe refuses an invalid region")
{
    eb::AzureProbe probe(std::make_unique<StatusHttp>(200));
    auto key = eb::SecretString::copy_of(L"SECRETKEY0123456789abcdefghijklmn");
    REQUIRE(key.has_value());
    CHECK_FALSE(probe.start("Bad Region", std::move(*key), [](int) {}));
}
