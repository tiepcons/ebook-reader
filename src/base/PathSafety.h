#pragma once
#include <filesystem>

namespace eb {

// False for device namespaces (\\.\, \\?\ not followed by a drive letter or UNC, GLOBALROOT) and for reserved
// DOS device names (CON, NUL, COM1 ...). Mirrors is_device_prefix / is_reserved_name in libs/eb_mupdf/eb_open.c.
bool is_safe_book_path(const std::filesystem::path& p) noexcept;

}
