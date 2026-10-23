#pragma once
#include "Result.h"

#include <string>
#include <string_view>

namespace eb {

Result<std::wstring> utf8_to_wide(std::string_view s);
Result<std::string> wide_to_utf8(std::wstring_view s);

std::wstring fold_for_search(std::wstring_view s) noexcept;
// Unicode lower-casing (invariant locale); returns empty on allocation failure.
std::wstring lower_case(std::wstring_view s) noexcept;

}
