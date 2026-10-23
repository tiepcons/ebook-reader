#pragma once
#include "Handle.h"
#include "Result.h"

#include <filesystem>

namespace eb {

Result<void> secure_dll_search(const std::filesystem::path& libs_dir);

Result<UniqueModule> load_checked(const std::filesystem::path& abs_dll, const char* version_export, int expected);

}
