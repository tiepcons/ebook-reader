#pragma once
#include "base/Result.h"

#include <cstddef>
#include <span>
#include <string>

namespace eb {

inline bool is_high(wchar_t c) noexcept { return c >= 0xD800 && c <= 0xDBFF; }
inline bool is_low(wchar_t c) noexcept { return c >= 0xDC00 && c <= 0xDFFF; }

inline bool is_control(wchar_t c) noexcept
{
    return c < 0x20 || (c >= 0x7F && c <= 0x9F) || c == 0xFFFE || c == 0xFFFF || c == 0xFEFF;
}

// Detects the encoding, strips unsafe characters, normalizes line breaks to LF and text to NFC.
Result<std::wstring> decode_text(std::span<const std::byte> raw);

}
