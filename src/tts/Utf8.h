#pragma once
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace eb::u8 {

struct Cp {
    char32_t cp;
    uint32_t len;
};

inline bool is_cont(std::string_view s, size_t i) noexcept
{
    return i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80;
}

inline Cp decode(std::string_view s, size_t i) noexcept
{
    const auto b0 = static_cast<unsigned char>(s[i]);
    if (b0 < 0x80)
        return {b0, 1};
    auto low = [&](size_t k) { return static_cast<char32_t>(static_cast<unsigned char>(s[i + k]) & 0x3F); };
    if (b0 >= 0xC2 && b0 <= 0xDF && is_cont(s, i + 1))
        return {(static_cast<char32_t>(b0 & 0x1F) << 6) | low(1), 2};
    if (b0 >= 0xE0 && b0 <= 0xEF && is_cont(s, i + 1) && is_cont(s, i + 2))
        return {(static_cast<char32_t>(b0 & 0x0F) << 12) | (low(1) << 6) | low(2), 3};
    if (b0 >= 0xF0 && b0 <= 0xF4 && is_cont(s, i + 1) && is_cont(s, i + 2) && is_cont(s, i + 3))
        return {(static_cast<char32_t>(b0 & 0x07) << 18) | (low(1) << 12) | (low(2) << 6) | low(3), 4};
    return {0xFFFD, 1};
}

inline bool is_space(char32_t c) noexcept
{
    return c == ' ' || (c >= '\t' && c <= '\r') || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A)
        || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

inline bool is_combining(char32_t c) noexcept
{
    return c >= 0x0300 && c <= 0x036F;
}

inline WORD ctype(char32_t c) noexcept
{
    if (c >= 0x10000)
        return 0;
    const wchar_t w = static_cast<wchar_t>(c);
    WORD t = 0;
    return GetStringTypeW(CT_CTYPE1, &w, 1, &t) ? t : static_cast<WORD>(0);
}

inline bool is_lower(char32_t c) noexcept
{
    return c < 0x80 ? (c >= 'a' && c <= 'z') : (ctype(c) & C1_LOWER) != 0;
}

inline bool is_upper(char32_t c) noexcept
{
    return c < 0x80 ? (c >= 'A' && c <= 'Z') : (ctype(c) & C1_UPPER) != 0;
}

inline bool is_alpha(char32_t c) noexcept
{
    if (c < 0x80)
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    return is_combining(c) || (ctype(c) & C1_ALPHA) != 0;
}

}
