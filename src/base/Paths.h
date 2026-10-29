#pragma once
#include "Result.h"

#include <filesystem>

namespace eb {

struct AppPaths {
    std::filesystem::path exe_dir, ebooks, data, covers, tts_cache, crash, logs, libs, fonts, voices, cache;
};

using WritableProbe = bool (*)(const std::filesystem::path&) noexcept;

bool dir_is_writable(const std::filesystem::path& dir) noexcept;

Result<AppPaths> resolve_paths(const std::filesystem::path& exe_dir);
Result<AppPaths> resolve_paths(
    const std::filesystem::path& exe_dir, const std::filesystem::path& local_root, WritableProbe probe);

}
