#include "Db.h"

#include "base/Log.h"
#include "base/Text.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <new>

namespace eb {
namespace {

ErrorCode map_code(int rc) noexcept
{
    switch (rc & 0xff) {
    case SQLITE_NOMEM:
        return ErrorCode::NoMemory;
    case SQLITE_CANTOPEN:
        return ErrorCode::NotFound;
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
    case SQLITE_READONLY:
    case SQLITE_PERM:
        return ErrorCode::Access;
    case SQLITE_CORRUPT:
    case SQLITE_NOTADB:
    case SQLITE_CONSTRAINT:
        return ErrorCode::Format;
    case SQLITE_TOOBIG:
    case SQLITE_RANGE:
        return ErrorCode::Limit;
    case SQLITE_IOERR:
    case SQLITE_FULL:
        return ErrorCode::Io;
    default:
        return ErrorCode::Internal;
    }
}

std::unexpected<Error> db_fail(sqlite3* db, int rc) noexcept
{
    try {
        std::wstring detail;
        if (db != nullptr) {
            if (auto w = utf8_to_wide(sqlite3_errmsg(db)))
                detail = std::move(*w);
        }
        return fail(map_code(rc), std::move(detail));
    } catch (...) {
        return std::unexpected<Error>(Error{map_code(rc), {}});
    }
}

Result<void> check(sqlite3* db, int rc) noexcept
{
    if (rc == SQLITE_OK)
        return {};
    return db_fail(db, rc);
}

Result<void> apply_pragmas(Db& db, int busy_timeout_ms) noexcept
{
    // busy_timeout first: switching to WAL takes a lock and must wait for concurrent openers, not fail with BUSY.
    sqlite3_busy_timeout(db.raw(), std::clamp(busy_timeout_ms, 0, 60000));
    auto mode = db.prepare("PRAGMA journal_mode=WAL");
    if (!mode)
        return std::unexpected(std::move(mode.error()));
    if (auto step = mode->step(); !step)
        return std::unexpected(std::move(step.error()));
    if (mode->column_text(0) != "wal")
        log::write("WARN", "database is not in WAL mode");
    for (const char* pragma : {"PRAGMA synchronous=NORMAL", "PRAGMA foreign_keys=ON"}) {
        if (auto r = db.exec(pragma); !r)
            return r;
    }
    return {};
}

}

void DbCloser::operator()(sqlite3* db) const noexcept
{
    sqlite3_close_v2(db);
}

void StmtFinalizer::operator()(sqlite3_stmt* st) const noexcept
{
    sqlite3_finalize(st);
}

Result<Db> Db::open(const std::filesystem::path& file, int busy_timeout_ms) noexcept
{
    try {
        auto utf8 = wide_to_utf8(file.wstring());
        if (!utf8)
            return std::unexpected(std::move(utf8.error()));
        sqlite3* raw = nullptr;
        const int rc = sqlite3_open_v2(utf8->c_str(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
        Db db(raw);
        if (rc != SQLITE_OK)
            return db_fail(raw, rc);
        if (auto r = apply_pragmas(db, busy_timeout_ms); !r)
            return std::unexpected(std::move(r.error()));
        return db;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::NoMemory);
    } catch (...) {
        return fail(ErrorCode::Internal);
    }
}

Result<void> Db::exec(const char* sql) noexcept
{
    return check(db_.get(), sqlite3_exec(db_.get(), sql, nullptr, nullptr, nullptr));
}

Result<Stmt> Db::prepare(std::string_view sql) noexcept
{
    if (sql.size() > static_cast<size_t>(INT_MAX))
        return fail(ErrorCode::Limit, L"sql too long");
    sqlite3_stmt* st = nullptr;
    const char* tail = nullptr;
    const int rc = sqlite3_prepare_v2(db_.get(), sql.data(), static_cast<int>(sql.size()), &st, &tail);
    Stmt s(st);
    if (rc != SQLITE_OK)
        return db_fail(db_.get(), rc);
    if (st == nullptr)
        return fail(ErrorCode::Format, L"empty statement");
    const char* end = sql.data() + sql.size();
    if (std::find_if(tail, end, [](char c) { return !std::isspace(static_cast<unsigned char>(c)) && c != '\0'; })
        != end)
        return fail(ErrorCode::Format, L"trailing text after statement");
    return s;
}

Result<void> Stmt::bind_int(int index, int64_t v) noexcept
{
    return check(sqlite3_db_handle(st_.get()), sqlite3_bind_int64(st_.get(), index, v));
}

Result<void> Stmt::bind_double(int index, double v) noexcept
{
    return check(sqlite3_db_handle(st_.get()), sqlite3_bind_double(st_.get(), index, v));
}

Result<void> Stmt::bind_text(int index, std::string_view utf8) noexcept
{
    const char* data = utf8.data() != nullptr ? utf8.data() : "";
    return check(sqlite3_db_handle(st_.get()),
        sqlite3_bind_text64(st_.get(), index, data, utf8.size(), SQLITE_TRANSIENT, SQLITE_UTF8));
}

Result<void> Stmt::bind_null(int index) noexcept
{
    return check(sqlite3_db_handle(st_.get()), sqlite3_bind_null(st_.get(), index));
}

Result<Step> Stmt::step() noexcept
{
    const int rc = sqlite3_step(st_.get());
    if (rc == SQLITE_ROW)
        return Step::Row;
    if (rc == SQLITE_DONE)
        return Step::Done;
    return db_fail(sqlite3_db_handle(st_.get()), rc);
}

Result<void> Stmt::reset() noexcept
{
    sqlite3_clear_bindings(st_.get());
    return check(sqlite3_db_handle(st_.get()), sqlite3_reset(st_.get()));
}

bool Stmt::is_null(int col) const noexcept
{
    return sqlite3_column_type(st_.get(), col) == SQLITE_NULL;
}

int64_t Stmt::column_int(int col) const noexcept
{
    return sqlite3_column_int64(st_.get(), col);
}

double Stmt::column_double(int col) const noexcept
{
    return sqlite3_column_double(st_.get(), col);
}

std::string_view Stmt::column_text(int col) const noexcept
{
    const unsigned char* p = sqlite3_column_text(st_.get(), col);
    if (p == nullptr)
        return {};
    return {reinterpret_cast<const char*>(p), static_cast<size_t>(sqlite3_column_bytes(st_.get(), col))};
}

Result<Tx> Tx::begin(Db& db) noexcept
{
    if (auto r = db.exec("BEGIN IMMEDIATE"); !r)
        return std::unexpected(std::move(r.error()));
    return Tx(&db);
}

Result<void> Tx::commit() noexcept
{
    if (db_ == nullptr)
        return fail(ErrorCode::Internal, L"transaction finished");
    auto r = db_->exec("COMMIT");
    if (r)
        db_ = nullptr;
    return r;
}

Tx::~Tx()
{
    if (db_ != nullptr && sqlite3_get_autocommit(db_->raw()) == 0 && !db_->exec("ROLLBACK"))
        log::error("rollback failed");
}

}
