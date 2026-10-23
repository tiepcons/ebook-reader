#include "Text.h"

#include "WinError.h"

#include <limits>

namespace eb {
namespace {

constexpr size_t kMaxChars = static_cast<size_t>(std::numeric_limits<int>::max());

bool is_combining(wchar_t c) noexcept
{
    return c >= 0x0300 && c <= 0x036F;
}

bool is_high_surrogate(wchar_t c) noexcept
{
    return c >= 0xD800 && c <= 0xDBFF;
}

bool is_low_surrogate(wchar_t c) noexcept
{
    return c >= 0xDC00 && c <= 0xDFFF;
}

std::wstring map_lower(const std::wstring& s)
{
    if (s.empty() || s.size() > kMaxChars)
        return s;
    std::wstring out(s.size(), L'\0');
    const int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.data(), static_cast<int>(s.size()),
        out.data(), static_cast<int>(out.size()), nullptr, nullptr, 0);
    if (n <= 0)
        return s;
    out.resize(static_cast<size_t>(n));
    return out;
}

std::wstring decompose(const std::wstring& s)
{
    if (s.empty() || s.size() > kMaxChars)
        return s;
    int cap = NormalizeString(NormalizationD, s.data(), static_cast<int>(s.size()), nullptr, 0);
    for (int attempt = 0; attempt < 4 && cap > 0; ++attempt) {
        std::wstring out(static_cast<size_t>(cap), L'\0');
        const int n = NormalizeString(NormalizationD, s.data(), static_cast<int>(s.size()), out.data(), cap);
        if (n > 0) {
            out.resize(static_cast<size_t>(n));
            return out;
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            break;
        cap *= 2;
    }
    return s;
}

}

Result<std::wstring> utf8_to_wide(std::string_view s)
{
    if (s.empty())
        return std::wstring();
    if (s.size() > kMaxChars)
        return fail(ErrorCode::Limit, L"utf8 input too large");
    const int len = static_cast<int>(s.size());
    const char* const src = s.data();
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, len, nullptr, 0);
    if (n <= 0)
        return fail(ErrorCode::Format, L"invalid utf-8");
    std::wstring out(static_cast<size_t>(n), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, len, out.data(), n) != n)
        return fail(ErrorCode::Format, L"invalid utf-8");
    return out;
}

Result<std::string> wide_to_utf8(std::wstring_view s)
{
    if (s.empty())
        return std::string();
    if (s.size() > kMaxChars)
        return fail(ErrorCode::Limit, L"wide input too large");
    const int len = static_cast<int>(s.size());
    const wchar_t* const src = s.data();
    const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, src, len, nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return fail(ErrorCode::Format, L"invalid utf-16");
    std::string out(static_cast<size_t>(n), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, src, len, out.data(), n, nullptr, nullptr) != n)
        return fail(ErrorCode::Format, L"invalid utf-16");
    return out;
}

std::wstring fold_for_search(std::wstring_view s) noexcept
{
    try {
        std::wstring in(s);
        for (size_t i = 0; i < in.size(); ++i) {
            wchar_t& c = in[i];
            if (c == 0x0111 || c == 0x0110)
                c = L'd';
            else if (is_high_surrogate(c) && i + 1 < in.size() && is_low_surrogate(in[i + 1]))
                ++i;
            else if (is_high_surrogate(c) || is_low_surrogate(c))
                c = 0xFFFD;
        }
        std::wstring out;
        out.reserve(in.size());
        for (const wchar_t c : decompose(in)) {
            if (!is_combining(c))
                out.push_back(c);
        }
        return map_lower(out);
    } catch (...) {
        return {};
    }
}

}
