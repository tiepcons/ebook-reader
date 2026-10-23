#include "Kv.h"

#include "base/Log.h"

namespace eb {

std::optional<std::string> kv_get(Db& db, std::string_view key) noexcept
{
    try {
        auto st = db.prepare("SELECT value FROM kv WHERE key=?1");
        if (!st || !st->bind_text(1, key))
            return std::nullopt;
        auto step = st->step();
        if (!step || *step != Step::Row || st->is_null(0))
            return std::nullopt;
        return std::string(st->column_text(0));
    } catch (...) {
        return std::nullopt;
    }
}

bool kv_set(Db& db, std::string_view key, std::string_view value) noexcept
{
    try {
        auto st = db.prepare("INSERT INTO kv(key,value) VALUES(?1,?2) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
        if (!st || !st->bind_text(1, key) || !st->bind_text(2, value)) {
            log::error("kv_set failed for {}", key);
            return false;
        }
        if (st->step())
            return true;
        log::error("kv_set failed for {}", key);
        return false;
    } catch (...) {
        return false;
    }
}

}
