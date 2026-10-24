#pragma once
#include "Db.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace eb {

inline constexpr uint64_t kDefaultAzureQuota = 500000;

struct UsageStatus {
    uint64_t used = 0;
    uint64_t quota = kDefaultAzureQuota;
    double percent_used = 0;
    double percent_left = 100;
    int64_t reset_utc_unix = 0;
    bool over_quota = false;
};

enum class Warn : unsigned char { None, P80, P95, P100 };

// Characters of the plain text sent to Azure; CJK, kana and hangul count 2.
uint64_t billable_chars(std::string_view utf8_plain_text) noexcept;
std::string month_utc(int64_t unix_seconds);
std::wstring format_status_vi(const UsageStatus& s) noexcept;

class AzureUsage {
public:
    explicit AzureUsage(Db& db) noexcept : db_(&db) {}

    Result<void> add(int64_t now_unix, uint64_t chars) noexcept;
    Result<UsageStatus> status(int64_t now_unix) noexcept;
    Result<void> calibrate(int64_t now_unix, uint64_t used_this_month) noexcept;
    Result<void> reset_month(int64_t now_unix) noexcept;
    Result<Warn> check_warning(int64_t now_unix) noexcept;

private:
    Db* db_;
};

}
