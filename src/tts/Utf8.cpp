#include "Utf8.h"

#include <windows.h>

namespace eb::u8 {
namespace {

WORD ctype(char32_t c) noexcept
{
    if (c >= 0x10000)
        return 0;
    const wchar_t w = static_cast<wchar_t>(c);
    WORD t = 0;
    return GetStringTypeW(CT_CTYPE1, &w, 1, &t) ? t : static_cast<WORD>(0);
}

}

bool is_lower(char32_t c) noexcept
{
    return c < 0x80 ? (c >= 'a' && c <= 'z') : (ctype(c) & C1_LOWER) != 0;
}

bool is_upper(char32_t c) noexcept
{
    return c < 0x80 ? (c >= 'A' && c <= 'Z') : (ctype(c) & C1_UPPER) != 0;
}

bool is_alpha(char32_t c) noexcept
{
    if (c < 0x80)
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    return is_combining(c) || (ctype(c) & C1_ALPHA) != 0;
}

}
