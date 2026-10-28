#include "AzureSettings.h"

#include "tts/azure/AzureProto.h"

#include <algorithm>

namespace eb {
namespace {

constexpr size_t kMaxCountDigits = 13;

bool is_space(wchar_t c) noexcept
{
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
}

}

std::optional<std::string> parse_region(std::wstring_view text)
{
    while (!text.empty() && is_space(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && is_space(text.back()))
        text.remove_suffix(1);
    std::string out;
    for (const wchar_t c : text) {
        if (c > 0x7E)
            return std::nullopt;
        out.push_back(c >= L'A' && c <= L'Z' ? static_cast<char>(c - L'A' + 'a') : static_cast<char>(c));
    }
    if (out.empty() || valid_azure_region(out))
        return out;
    return std::nullopt;
}

std::optional<uint64_t> parse_char_count(std::wstring_view text, uint64_t min, uint64_t max) noexcept
{
    uint64_t v = 0;
    size_t digits = 0;
    for (const wchar_t c : text) {
        if (c == L'.' || c == L',' || is_space(c))
            continue;
        if (c < L'0' || c > L'9' || ++digits > kMaxCountDigits)
            return std::nullopt;
        v = v * 10 + static_cast<uint64_t>(c - L'0');
    }
    if (digits == 0 || v < min || v > max)
        return std::nullopt;
    return v;
}

AzureTier parse_tier(std::string_view text) noexcept
{
    return text == "S0" ? AzureTier::S0 : AzureTier::F0;
}

const char* tier_name(AzureTier t) noexcept
{
    return t == AzureTier::S0 ? "S0" : "F0";
}

int percent_left_whole(const UsageStatus& s) noexcept
{
    return std::clamp(static_cast<int>(s.percent_left), 0, 100);
}

std::wstring bar_quota_text(const UsageStatus& s)
{
    return L"Azure " + std::to_wstring(percent_left_whole(s)) + L"%";
}

std::wstring tray_quota_text(const UsageStatus& s)
{
    return L"Azure còn " + std::to_wstring(percent_left_whole(s)) + L"%";
}

std::wstring warn_text(Warn w)
{
    switch (w) {
    case Warn::P80:
        return L"Azure: đã dùng 80% hạn mức ký tự miễn phí của tháng này.";
    case Warn::P95:
        return L"Azure: đã dùng 95% hạn mức tháng này, sắp hết.";
    case Warn::P100:
        return L"Azure: đã hết hạn mức tháng này. Chuyển sang giọng Windows.";
    default:
        return {};
    }
}

std::wstring probe_text(int status)
{
    if (status == 200)
        return L"Kết nối thành công. Bấm OK rồi mở lại hộp thoại để chọn giọng Azure.";
    if (status == 401 || status == 403)
        return L"Azure từ chối khóa (lỗi " + std::to_wstring(status) + L"): khóa sai, hết hạn hoặc không đúng vùng.";
    if (status == 0)
        return L"Không kết nối được tới Azure. Kiểm tra mạng và tên vùng.";
    if (status == 404)
        return L"Không tìm thấy vùng này (lỗi 404). Kiểm tra lại tên vùng.";
    return L"Azure trả lỗi " + std::to_wstring(status) + L".";
}

}
