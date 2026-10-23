#pragma once
#include "Result.h"

#include <cstddef>
#include <filesystem>
#include <span>

namespace eb {

Result<void> write_atomic(const std::filesystem::path& target, std::span<const std::byte> data);

}
