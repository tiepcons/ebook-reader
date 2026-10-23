#include "Schema.h"

namespace eb {
namespace {

constexpr const char* kV1 = R"sql(
CREATE TABLE books(
  id TEXT PRIMARY KEY, path TEXT NOT NULL UNIQUE, folder TEXT NOT NULL, title TEXT, author TEXT,
  title_nd TEXT, author_nd TEXT, lang TEXT, fmt TEXT NOT NULL, size INTEGER NOT NULL,
  mtime INTEGER NOT NULL, added_at INTEGER NOT NULL, last_opened INTEGER,
  status INTEGER NOT NULL DEFAULT 0);
CREATE TABLE progress(
  book_id TEXT PRIMARY KEY REFERENCES books(id) ON DELETE CASCADE, bookmark INTEGER NOT NULL,
  percent REAL NOT NULL, tts_char INTEGER NOT NULL DEFAULT -1, updated_at INTEGER NOT NULL);
CREATE TABLE bookmarks(
  id INTEGER PRIMARY KEY, book_id TEXT NOT NULL REFERENCES books(id) ON DELETE CASCADE,
  bookmark INTEGER NOT NULL, label TEXT, created_at INTEGER NOT NULL);
CREATE TABLE tts_usage(month TEXT PRIMARY KEY, chars INTEGER NOT NULL);
CREATE TABLE kv(key TEXT PRIMARY KEY, value TEXT);
CREATE INDEX books_folder ON books(folder);
CREATE INDEX books_title_nd ON books(title_nd);
CREATE INDEX books_author_nd ON books(author_nd);
CREATE INDEX books_last_opened ON books(last_opened);
PRAGMA user_version = 1;
)sql";

}

Result<int> user_version(Db& db) noexcept
{
    auto st = db.prepare("PRAGMA user_version");
    if (!st)
        return std::unexpected(std::move(st.error()));
    auto step = st->step();
    if (!step)
        return std::unexpected(std::move(step.error()));
    return static_cast<int>(st->column_int(0));
}

Result<void> migrate(Db& db) noexcept
{
    auto v = user_version(db);
    if (!v)
        return std::unexpected(std::move(v.error()));
    if (*v > kSchemaVersion)
        return fail(ErrorCode::Format, L"database is newer than this program");
    if (*v == kSchemaVersion)
        return {};
    auto tx = Tx::begin(db);
    if (!tx)
        return std::unexpected(std::move(tx.error()));
    if (auto r = db.exec(kV1); !r)
        return r;
    return tx->commit();
}

}
