#pragma once
#include "lib/Db.h"

#include <optional>
#include <string>
#include <string_view>

namespace eb {

std::optional<std::string> kv_get(Db& db, std::string_view key) noexcept;
bool kv_set(Db& db, std::string_view key, std::string_view value) noexcept;

}
