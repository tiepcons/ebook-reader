#pragma once
#include "Db.h"
#include "base/CrashGuard.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <span>
#include <string>

namespace eb {

struct BookMeta {
    std::wstring title, author, lang, fmt;
};

using MetaFn = std::function<Result<BookMeta>(const std::filesystem::path&)>;

struct ScanStats {
    int added = 0, updated = 0, moved = 0, missing = 0, bad = 0, unchanged = 0;
};

inline constexpr size_t kMaxScanFiles = 200'000;

// Runs on the caller's thread. Never follows reparse points. Rows are never deleted.
// Directories named in exclude_dirs (case-insensitive) are skipped at any depth.
// Books are marked missing only under roots that were fully enumerated.
Result<ScanStats> scan_library(Db& db, std::span<const std::filesystem::path> roots, const MetaFn& meta,
    CrashLoopGuard& guard, const std::atomic<bool>& cancel,
    const std::function<void(int done, int total)>& progress = {},
    std::span<const std::wstring> exclude_dirs = {}) noexcept;

namespace scan_path {

// "\\?\C:\x" -> "C:\x", "\\?\UNC\h\s" -> "\\h\s".
std::wstring strip_long_prefix(std::wstring_view p);
// Directory + wildcard for FindFirstFileEx; keeps a drive root's trailing backslash.
std::wstring pattern(std::wstring_view dir);
std::wstring join(std::wstring_view dir, std::wstring_view name);

}

}
