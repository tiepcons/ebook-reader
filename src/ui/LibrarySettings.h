#pragma once
#include "lib/Db.h"

#include <windows.h>

#include <filesystem>
#include <vector>

namespace eb {

inline constexpr size_t kMaxExtraRoots = 32;

std::vector<std::filesystem::path> load_extra_roots(Db& db);
bool scan_on_start(Db& db) noexcept;

// Modal. Returns true when the user confirmed a change to the set of roots.
bool show_library_settings(HINSTANCE inst, HWND owner, Db& db) noexcept;

}
