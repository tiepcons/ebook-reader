#pragma once
#include "Result.h"

#include <filesystem>
#include <string>

namespace eb {

Result<std::string> book_id(const std::filesystem::path& file);

}
