#pragma once
#include "Db.h"

namespace eb {

inline constexpr int kSchemaVersion = 1;

Result<int> user_version(Db& db) noexcept;
Result<void> migrate(Db& db) noexcept;

}
