#pragma once
#include "base/Result.h"
#include "lib/Db.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eb {

struct Progress {
    uint64_t mark = 0; // eb_bookmark
    double percent = 0.0;
    int64_t tts_char = -1;
    int64_t updated_at = 0;
};

struct Bookmark {
    int64_t id = 0;
    uint64_t mark = 0;
    std::wstring label;
    int64_t created_at = 0;
};

inline constexpr size_t kMaxBookmarkLabel = 200;

class ProgressStore {
public:
    explicit ProgressStore(Db& db) noexcept : db_(&db) {}

    Result<std::optional<Progress>> load(std::string_view book_id) noexcept;
    Result<void> save(std::string_view book_id, const Progress& p) noexcept;

private:
    Db* db_;
};

// Coalesces frequent position updates for one open book; the caller drives the clock.
class ProgressSaver {
public:
    static constexpr int64_t kIntervalMs = 1000;

    ProgressSaver(ProgressStore& store, std::string book_id) noexcept : store_(&store), book_id_(std::move(book_id)) {}

    void update(const Progress& p) noexcept;
    void tick(int64_t now_ms) noexcept;
    void flush() noexcept;

private:
    bool save_pending() noexcept;

    ProgressStore* store_;
    std::string book_id_;
    std::optional<Progress> pending_;
    std::optional<int64_t> anchor_ms_;
};

Result<int64_t> add_bookmark(Db& db, std::string_view book_id, uint64_t mark, std::wstring_view label) noexcept;
Result<std::vector<Bookmark>> list_bookmarks(Db& db, std::string_view book_id) noexcept;
Result<void> remove_bookmark(Db& db, int64_t id) noexcept;

}
