#pragma once
#include "base/Result.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <utility>

struct sqlite3;
struct sqlite3_stmt;

namespace eb {

struct DbCloser {
    void operator()(sqlite3* db) const noexcept;
};
struct StmtFinalizer {
    void operator()(sqlite3_stmt* st) const noexcept;
};

enum class Step : unsigned char { Row, Done };

class Stmt {
public:
    explicit Stmt(sqlite3_stmt* st) noexcept : st_(st) {}

    Result<void> bind_int(int index, int64_t v) noexcept;
    Result<void> bind_double(int index, double v) noexcept;
    Result<void> bind_text(int index, std::string_view utf8) noexcept;
    Result<void> bind_null(int index) noexcept;
    Result<Step> step() noexcept;
    Result<void> reset() noexcept;

    [[nodiscard]] bool is_null(int col) const noexcept;
    [[nodiscard]] int64_t column_int(int col) const noexcept;
    [[nodiscard]] double column_double(int col) const noexcept;
    [[nodiscard]] std::string_view column_text(int col) const noexcept;

private:
    std::unique_ptr<sqlite3_stmt, StmtFinalizer> st_;
};

class Db {
public:
    static Result<Db> open(const std::filesystem::path& file) noexcept;

    Result<void> exec(const char* sql) noexcept;
    Result<Stmt> prepare(std::string_view sql) noexcept;
    [[nodiscard]] sqlite3* raw() const noexcept { return db_.get(); }

private:
    explicit Db(sqlite3* db) noexcept : db_(db) {}
    std::unique_ptr<sqlite3, DbCloser> db_;
};

class Tx {
public:
    static Result<Tx> begin(Db& db) noexcept;
    Tx(Tx&& o) noexcept : db_(std::exchange(o.db_, nullptr)) {}
    Tx(const Tx&) = delete;
    Tx& operator=(const Tx&) = delete;
    Tx& operator=(Tx&&) = delete;
    ~Tx();

    Result<void> commit() noexcept;

private:
    explicit Tx(Db* db) noexcept : db_(db) {}
    Db* db_;
};

}
