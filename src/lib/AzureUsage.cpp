#include "AzureUsage.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <new>
#include <optional>

namespace eb {
namespace {

constexpr uint64_t kMaxCount = 1000000000000000ULL;
constexpr uint64_t kMaxQuota = 1000000000000ULL;
constexpr int64_t kSecondsPerDay = 86400;

#define EB_TRY(expr)                                                                                                   \
    if (auto eb_r = (expr); !eb_r)                                                                                     \
    return std::unexpected(std::move(eb_r.error()))

struct Civil {
    int64_t year;
    int month;
    int day;
};

int64_t floor_div(int64_t a, int64_t b) noexcept
{
    const int64_t q = a / b;
    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

int64_t days_from_civil(int64_t y, int m, int d) noexcept
{
    y -= m <= 2 ? 1 : 0;
    const int64_t era = floor_div(y, 400);
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

Civil civil_from_days(int64_t z) noexcept
{
    z += 719468;
    const int64_t era = floor_div(z, 146097);
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    const int m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    return {yoe + era * 400 + (m <= 2 ? 1 : 0), m, d};
}

Civil civil_of(int64_t unix_seconds) noexcept
{
    return civil_from_days(floor_div(unix_seconds, kSecondsPerDay));
}

int64_t next_month_start(int64_t unix_seconds) noexcept
{
    const Civil c = civil_of(unix_seconds);
    const int64_t y = c.month == 12 ? c.year + 1 : c.year;
    return days_from_civil(y, c.month == 12 ? 1 : c.month + 1, 1) * kSecondsPerDay;
}

bool is_double_width(char32_t cp) noexcept
{
    return (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x2E80 && cp <= 0x2FDF) || (cp >= 0x3040 && cp <= 0x30FF)
        || (cp >= 0x3130 && cp <= 0x318F) || (cp >= 0x31F0 && cp <= 0x31FF) || (cp >= 0x3400 && cp <= 0x4DBF)
        || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xF900 && cp <= 0xFAFF)
        || (cp >= 0x20000 && cp <= 0x3FFFF);
}

// Decodes one strictly valid UTF-8 sequence; returns 0 length when invalid.
size_t decode_one(std::string_view s, size_t i, char32_t& cp) noexcept
{
    const auto b = [&](size_t k) { return static_cast<unsigned char>(s[i + k]); };
    const unsigned char c = b(0);
    size_t len = 0;
    char32_t min = 0;
    if (c < 0x80) {
        cp = c;
        return 1;
    }
    if (c >= 0xC2 && c <= 0xDF) {
        len = 2, cp = c & 0x1Fu, min = 0x80;
    } else if (c >= 0xE0 && c <= 0xEF) {
        len = 3, cp = c & 0x0Fu, min = 0x800;
    } else if (c >= 0xF0 && c <= 0xF4) {
        len = 4, cp = c & 0x07u, min = 0x10000;
    } else {
        return 0;
    }
    if (i + len > s.size())
        return 0;
    for (size_t k = 1; k < len; ++k) {
        if ((b(k) & 0xC0) != 0x80)
            return 0;
        cp = (cp << 6) | (b(k) & 0x3Fu);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        return 0;
    return len;
}

uint64_t clamp_count(int64_t v) noexcept
{
    return v < 0 ? 0 : std::min<uint64_t>(static_cast<uint64_t>(v), kMaxCount);
}

Result<std::optional<std::string>> kv_get(Db& db, std::string_view key)
{
    auto st = db.prepare("SELECT value FROM kv WHERE key=?1");
    if (!st)
        return std::unexpected(std::move(st.error()));
    EB_TRY(st->bind_text(1, key));
    auto step = st->step();
    if (!step)
        return std::unexpected(std::move(step.error()));
    if (*step == Step::Done || st->is_null(0))
        return std::optional<std::string>{};
    return std::optional<std::string>{std::string(st->column_text(0))};
}

uint64_t parse_count(const std::optional<std::string>& text) noexcept
{
    uint64_t v = 0;
    if (!text || std::from_chars(text->data(), text->data() + text->size(), v).ec != std::errc{})
        return 0;
    return v;
}

Result<void> kv_put(Db& db, std::string_view key, std::string_view value)
{
    auto st = db.prepare("INSERT INTO kv(key,value) VALUES(?1,?2) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    if (!st)
        return std::unexpected(std::move(st.error()));
    EB_TRY(st->bind_text(1, key));
    EB_TRY(st->bind_text(2, value));
    auto step = st->step();
    if (!step)
        return std::unexpected(std::move(step.error()));
    return {};
}

Result<void> put_month(Db& db, std::string_view month, uint64_t chars, bool additive)
{
    const char* sql = additive ? "INSERT INTO tts_usage(month,chars) VALUES(?1,?2) ON CONFLICT(month) DO UPDATE SET "
                                 "chars=MIN(chars+excluded.chars,1000000000000000)"
                               : "INSERT INTO tts_usage(month,chars) VALUES(?1,?2) ON CONFLICT(month) DO UPDATE SET "
                                 "chars=excluded.chars";
    auto st = db.prepare(sql);
    if (!st)
        return std::unexpected(std::move(st.error()));
    EB_TRY(st->bind_text(1, month));
    EB_TRY(st->bind_int(2, static_cast<int64_t>(std::min(chars, kMaxCount))));
    auto step = st->step();
    if (!step)
        return std::unexpected(std::move(step.error()));
    return {};
}

Result<uint64_t> read_quota(Db& db)
{
    auto v = kv_get(db, "azure.quota");
    if (!v)
        return std::unexpected(std::move(v.error()));
    const uint64_t q = parse_count(*v);
    return q == 0 ? kDefaultAzureQuota : std::min(q, kMaxQuota);
}

Result<uint64_t> read_used(Db& db, std::string_view month)
{
    auto st = db.prepare("SELECT chars FROM tts_usage WHERE month=?1");
    if (!st)
        return std::unexpected(std::move(st.error()));
    EB_TRY(st->bind_text(1, month));
    auto step = st->step();
    if (!step)
        return std::unexpected(std::move(step.error()));
    return *step == Step::Done ? 0 : clamp_count(st->column_int(0));
}

Warn level_for(const UsageStatus& s) noexcept
{
    if (s.used * 100 >= s.quota * 100)
        return Warn::P100;
    if (s.used * 100 >= s.quota * 95)
        return Warn::P95;
    if (s.used * 100 >= s.quota * 80)
        return Warn::P80;
    return Warn::None;
}

std::wstring group_thousands(uint64_t v)
{
    std::wstring digits = std::to_wstring(v);
    std::wstring out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i != 0 && (digits.size() - i) % 3 == 0)
            out.push_back(L'.');
        out.push_back(digits[i]);
    }
    return out;
}

std::wstring percent_vi(double p)
{
    const auto tenths = static_cast<int64_t>(std::llround(p * 10));
    return std::to_wstring(tenths / 10) + L"," + std::to_wstring(tenths % 10);
}

std::wstring two_digits(int v)
{
    return (v < 10 ? L"0" : L"") + std::to_wstring(v);
}

}

uint64_t billable_chars(std::string_view s) noexcept
{
    uint64_t total = 0;
    for (size_t i = 0; i < s.size();) {
        char32_t cp = 0;
        const size_t len = decode_one(s, i, cp);
        total += len != 0 && is_double_width(cp) ? 2 : 1;
        i += len != 0 ? len : 1;
    }
    return total;
}

std::string month_utc(int64_t unix_seconds)
{
    const Civil c = civil_of(unix_seconds);
    const std::string m = std::to_string(c.month);
    return std::to_string(c.year) + (m.size() < 2 ? "-0" : "-") + m;
}

std::wstring format_status_vi(const UsageStatus& s) noexcept
{
    try {
        const Civil reset = civil_of(s.reset_utc_unix);
        return L"Đã dùng " + group_thousands(s.used) + L" / " + group_thousands(s.quota) + L" ký tự ("
            + percent_vi(s.percent_used) + L"%) · Còn " + percent_vi(s.percent_left) + L"% · Reset "
            + two_digits(reset.day) + L"/" + two_digits(reset.month) + L" (UTC)";
    } catch (...) {
        return {};
    }
}

Result<void> AzureUsage::add(int64_t now_unix, uint64_t chars) noexcept
{
    try {
        return put_month(*db_, month_utc(now_unix), chars, true);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::NoMemory);
    } catch (...) {
        return fail(ErrorCode::Internal);
    }
}

Result<UsageStatus> AzureUsage::status(int64_t now_unix) noexcept
{
    try {
        auto quota = read_quota(*db_);
        if (!quota)
            return std::unexpected(std::move(quota.error()));
        auto used = read_used(*db_, month_utc(now_unix));
        if (!used)
            return std::unexpected(std::move(used.error()));
        UsageStatus s;
        s.used = *used;
        s.quota = *quota;
        s.over_quota = s.used >= s.quota;
        const double pct = std::min(100.0, static_cast<double>(s.used) * 100.0 / static_cast<double>(s.quota));
        s.percent_used = std::round(pct * 10) / 10;
        s.percent_left = std::round((100.0 - pct) * 10) / 10;
        s.reset_utc_unix = next_month_start(now_unix);
        return s;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::NoMemory);
    } catch (...) {
        return fail(ErrorCode::Internal);
    }
}

Result<void> AzureUsage::calibrate(int64_t now_unix, uint64_t used_this_month) noexcept
{
    try {
        return put_month(*db_, month_utc(now_unix), used_this_month, false);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::NoMemory);
    } catch (...) {
        return fail(ErrorCode::Internal);
    }
}

Result<void> AzureUsage::reset_month(int64_t now_unix) noexcept
{
    try {
        const std::string month = month_utc(now_unix);
        auto tx = Tx::begin(*db_);
        if (!tx)
            return std::unexpected(std::move(tx.error()));
        EB_TRY(put_month(*db_, month, 0, false));
        auto st = db_->prepare("DELETE FROM kv WHERE key=?1");
        if (!st)
            return std::unexpected(std::move(st.error()));
        EB_TRY(st->bind_text(1, "azure.warned." + month));
        auto step = st->step();
        if (!step)
            return std::unexpected(std::move(step.error()));
        return tx->commit();
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::NoMemory);
    } catch (...) {
        return fail(ErrorCode::Internal);
    }
}

Result<Warn> AzureUsage::check_warning(int64_t now_unix) noexcept
{
    try {
        auto tx = Tx::begin(*db_);
        if (!tx)
            return std::unexpected(std::move(tx.error()));
        auto s = status(now_unix);
        if (!s)
            return std::unexpected(std::move(s.error()));
        const Warn level = level_for(*s);
        const std::string key = "azure.warned." + month_utc(now_unix);
        auto prev = kv_get(*db_, key);
        if (!prev)
            return std::unexpected(std::move(prev.error()));
        if (static_cast<uint64_t>(level) <= parse_count(*prev))
            return Warn::None;
        EB_TRY(kv_put(*db_, key, std::to_string(static_cast<int>(level))));
        EB_TRY(tx->commit());
        return level;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::NoMemory);
    } catch (...) {
        return fail(ErrorCode::Internal);
    }
}

}
