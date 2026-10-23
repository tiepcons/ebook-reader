#pragma once
#include "base/Result.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace eb {

inline constexpr std::size_t kMaxTxtBytes = std::size_t{64} << 20;

Result<std::string> txt_to_fb2(std::span<const std::byte> raw, std::wstring_view fallback_title) noexcept;

}
