#pragma once
#include "lib/AzureUsage.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace eb {

inline constexpr const char* kAzureTierKey = "tts.azure.tier";
inline constexpr const char* kAzureQuotaKey = "azure.quota";
inline constexpr uint64_t kMaxAzureQuota = 1000000000000ULL;

enum class AzureTier : unsigned char { F0, S0 };

// What the Azure part of the voice dialog shows.
struct AzureView {
    std::string region;
    AzureTier tier = AzureTier::F0;
    UsageStatus usage;
    bool usage_ok = false;
    bool has_key = false;
};

// What the dialog hands back on OK. `used` is set only when the user corrected the counter.
struct AzureEdits {
    std::string region;
    AzureTier tier = AzureTier::F0;
    uint64_t quota = kDefaultAzureQuota;
    std::optional<uint64_t> used;
    bool key_changed = false;
};

// Trimmed and lowercased; empty text is valid (Azure off). nullopt when it is not a host-name label of 3..32 [a-z0-9].
[[nodiscard]] std::optional<std::string> parse_region(std::wstring_view text);
// Digits with optional '.', ',' or space group separators; nullopt outside min..max or without a digit.
[[nodiscard]] std::optional<uint64_t> parse_char_count(std::wstring_view text, uint64_t min, uint64_t max) noexcept;
[[nodiscard]] AzureTier parse_tier(std::string_view text) noexcept;
[[nodiscard]] const char* tier_name(AzureTier t) noexcept;

// Whole percent of the free quota left, rounded down so that 99,9% never reads as 100%.
[[nodiscard]] int percent_left_whole(const UsageStatus& s) noexcept;
[[nodiscard]] std::wstring bar_quota_text(const UsageStatus& s);  // "Azure 75%"
[[nodiscard]] std::wstring tray_quota_text(const UsageStatus& s); // "Azure còn 75%"
[[nodiscard]] std::wstring warn_text(Warn w);
// HTTP status of the voices/list probe as a Vietnamese sentence.
[[nodiscard]] std::wstring probe_text(int status);

}
