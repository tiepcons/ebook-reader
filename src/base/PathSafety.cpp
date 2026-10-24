#include "PathSafety.h"

#include <array>
#include <string>
#include <string_view>

namespace eb {
namespace {

constexpr size_t kMaxReservedChars = 15;

bool is_sep(wchar_t c) noexcept
{
    return c == L'\\' || c == L'/';
}

wchar_t upper(wchar_t c) noexcept
{
    return (c >= L'a' && c <= L'z') ? static_cast<wchar_t>(c - 32) : c;
}

bool iequals(std::wstring_view a, std::wstring_view b) noexcept
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (upper(a[i]) != upper(b[i]))
            return false;
    }
    return true;
}

bool unsafe_prefix(std::wstring_view p) noexcept
{
    if (p.size() < 4 || !is_sep(p[0]) || !is_sep(p[1]))
        return false;
    if (p[2] == L'.' && is_sep(p[3]))
        return true;
    if (p[2] != L'?' || !is_sep(p[3]))
        return false;
    const std::wstring_view rest = p.substr(4);
    const bool drive = rest.size() >= 2 && rest[1] == L':' && upper(rest[0]) >= L'A' && upper(rest[0]) <= L'Z';
    const bool unc = rest.size() >= 4 && iequals(rest.substr(0, 3), L"UNC") && is_sep(rest[3]);
    return !drive && !unc;
}

bool reserved_name(std::wstring_view p) noexcept
{
    static constexpr std::array<std::wstring_view, 6> kNames = {L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"};
    const size_t cut = p.find_last_of(L"\\/");
    std::wstring_view base = cut == std::wstring_view::npos ? p : p.substr(cut + 1);
    if (cut == std::wstring_view::npos && base.size() >= 2 && base[1] == L':')
        base.remove_prefix(2);
    size_t n = 0;
    while (n < base.size() && base[n] != L'.' && base[n] != L':' && n < kMaxReservedChars)
        ++n;
    std::wstring_view name = base.substr(0, n);
    while (!name.empty() && name.back() == L' ')
        name.remove_suffix(1);
    for (const auto r : kNames) {
        if (iequals(name, r))
            return true;
    }
    return name.size() == 4 && (iequals(name.substr(0, 3), L"COM") || iequals(name.substr(0, 3), L"LPT")) && name[3] >= L'1'
        && name[3] <= L'9';
}

}

bool is_safe_book_path(const std::filesystem::path& p) noexcept
{
    try {
        const std::wstring w = p.wstring();
        return !w.empty() && !unsafe_prefix(w) && !reserved_name(w);
    } catch (...) {
        return false;
    }
}

}
