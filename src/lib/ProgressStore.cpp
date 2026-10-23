#include "ProgressStore.h"

#include "base/Log.h"
#include "base/Text.h"

#include <sqlite3.h>

#include <chrono>
#include <cmath>
#include <new>

namespace eb {
namespace {

constexpr size_t kMaxBookId = 256;

Result<void> check_id(std::string_view id) noexcept
{
    if (id.empty() || id.size() > kMaxBookId)
        return fail(ErrorCode::Limit, L"bad book id");
    return {};
}

int64_t unix_now() noexcept
{
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

bool is_control(wchar_t c) noexcept
{
    return c < 0x20 || (c >= 0x7f && c <= 0x9f);
}

std::wstring sanitize(std::wstring_view in)
{
    std::wstring out;
    for (const wchar_t c : in) {
        if (out.size() >= kMaxBookmarkLabel)
            break;
        if (!is_control(c))
            out.push_back(c);
    }
    if (!out.empty() && out.back() >= 0xD800 && out.back() <= 0xDBFF)
        out.pop_back();
    return out;
}

#define EB_TRY(expr)                                                                                                   \
    if (auto eb_r = (expr); !eb_r)                                                                                     \
    return std::unexpected(std::move(eb_r.error()))

Result<void> bind_label(Stmt& st, int index, std::wstring_view label)
{
    const std::wstring clean = sanitize(label);
    if (clean.empty())
        return st.bind_null(index);
    auto utf8 = wide_to_utf8(clean);
    if (!utf8)
        return std::unexpected(std::move(utf8.error()));
    return st.bind_text(index, *utf8);
}

Result<void> bind_progress(Stmt& st, std::string_view book_id, const Progress& p) noexcept
{
    EB_TRY(st.bind_text(1, book_id));
    EB_TRY(st.bind_int(2, static_cast<int64_t>(p.mark)));
    EB_TRY(st.bind_double(3, p.percent));
    EB_TRY(st.bind_int(4, p.tts_char));
    return st.bind_int(5, p.updated_at);
}

Result<void> run(Db& db, std::string_view sql, std::string_view book_id, int64_t at) noexcept
{
    auto st = db.prepare(sql);
    if (!st)
        return std::unexpected(std::move(st.error()));
    EB_TRY(st->bind_text(1, book_id));
    EB_TRY(st->bind_int(2, at));
    auto step = st->step();
    if (!step)
        return std::unexpected(std::move(step.error()));
    return {};
}

}

Result<std::optional<Progress>> ProgressStore::load(std::string_view book_id) noexcept
{
    EB_TRY(check_id(book_id));
    auto st = db_->prepare("SELECT bookmark,percent,tts_char,updated_at FROM progress WHERE book_id=?1");
    if (!st)
        return std::unexpected(std::move(st.error()));
    EB_TRY(st->bind_text(1, book_id));
    auto step = st->step();
    if (!step)
        return std::unexpected(std::move(step.error()));
    if (*step == Step::Done)
        return std::optional<Progress>{};
    return std::optional<Progress>{Progress{static_cast<uint64_t>(st->column_int(0)), st->column_double(1),
        st->column_int(2), st->column_int(3)}};
}

Result<void> ProgressStore::save(std::string_view book_id, const Progress& p) noexcept
{
    EB_TRY(check_id(book_id));
    if (!std::isfinite(p.percent))
        return fail(ErrorCode::Format, L"bad percent");
    auto tx = Tx::begin(*db_);
    if (!tx)
        return std::unexpected(std::move(tx.error()));
    auto up = db_->prepare("INSERT INTO progress(book_id,bookmark,percent,tts_char,updated_at) VALUES(?1,?2,?3,?4,?5) "
                           "ON CONFLICT(book_id) DO UPDATE SET bookmark=excluded.bookmark,percent=excluded.percent,"
                           "tts_char=excluded.tts_char,updated_at=excluded.updated_at");
    if (!up)
        return std::unexpected(std::move(up.error()));
    EB_TRY(bind_progress(*up, book_id, p));
    auto step = up->step();
    if (!step)
        return std::unexpected(std::move(step.error()));
    EB_TRY(run(*db_, "UPDATE books SET last_opened=?2 WHERE id=?1", book_id, p.updated_at));
    return tx->commit();
}

Result<std::optional<std::string>> ProgressStore::last_book() noexcept
{
    auto st = db_->prepare("SELECT p.book_id FROM progress p JOIN books b ON b.id=p.book_id WHERE b.status=0 "
                           "ORDER BY p.updated_at DESC LIMIT 1");
    if (!st)
        return std::unexpected(std::move(st.error()));
    auto step = st->step();
    if (!step)
        return std::unexpected(std::move(step.error()));
    if (*step == Step::Done)
        return std::optional<std::string>{};
    try {
        return std::optional<std::string>{std::string(st->column_text(0))};
    } catch (...) {
        return fail(ErrorCode::NoMemory);
    }
}

void ProgressSaver::update(const Progress& p) noexcept
{
    pending_ = p;
}

bool ProgressSaver::save_pending() noexcept
{
    if (!pending_)
        return true;
    auto r = store_->save(book_id_, *pending_);
    if (!r) {
        log::error("progress save failed (code {})", static_cast<int>(r.error().code));
        return false;
    }
    pending_.reset();
    return true;
}

void ProgressSaver::tick(int64_t now_ms) noexcept
{
    if (!pending_)
        return;
    if (!anchor_ms_) {
        anchor_ms_ = now_ms;
        return;
    }
    if (now_ms - *anchor_ms_ < kIntervalMs)
        return;
    save_pending();
    anchor_ms_ = now_ms;
}

void ProgressSaver::flush() noexcept
{
    save_pending();
    anchor_ms_.reset();
}

Result<int64_t> add_bookmark(Db& db, std::string_view book_id, uint64_t mark, std::wstring_view label) noexcept
{
    EB_TRY(check_id(book_id));
    try {
        auto st = db.prepare("INSERT INTO bookmarks(book_id,bookmark,label,created_at) VALUES(?1,?2,?3,?4)");
        if (!st)
            return std::unexpected(std::move(st.error()));
        EB_TRY(st->bind_text(1, book_id));
        EB_TRY(st->bind_int(2, static_cast<int64_t>(mark)));
        EB_TRY(bind_label(*st, 3, label));
        EB_TRY(st->bind_int(4, unix_now()));
        auto step = st->step();
        if (!step)
            return std::unexpected(std::move(step.error()));
        return static_cast<int64_t>(sqlite3_last_insert_rowid(db.raw()));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::NoMemory);
    } catch (...) {
        return fail(ErrorCode::Internal);
    }
}

Result<std::vector<Bookmark>> list_bookmarks(Db& db, std::string_view book_id) noexcept
{
    EB_TRY(check_id(book_id));
    try {
        auto st = db.prepare("SELECT id,bookmark,label,created_at FROM bookmarks WHERE book_id=?1 "
                             "ORDER BY bookmark,created_at,id");
        if (!st)
            return std::unexpected(std::move(st.error()));
        EB_TRY(st->bind_text(1, book_id));
        std::vector<Bookmark> out;
        for (;;) {
            auto step = st->step();
            if (!step)
                return std::unexpected(std::move(step.error()));
            if (*step == Step::Done)
                return out;
            Bookmark b{st->column_int(0), static_cast<uint64_t>(st->column_int(1)), {}, st->column_int(3)};
            if (!st->is_null(2)) {
                if (auto w = utf8_to_wide(st->column_text(2)))
                    b.label = std::move(*w);
            }
            out.push_back(std::move(b));
        }
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::NoMemory);
    } catch (...) {
        return fail(ErrorCode::Internal);
    }
}

Result<void> remove_bookmark(Db& db, int64_t id) noexcept
{
    auto st = db.prepare("DELETE FROM bookmarks WHERE id=?1");
    if (!st)
        return std::unexpected(std::move(st.error()));
    EB_TRY(st->bind_int(1, id));
    auto step = st->step();
    if (!step)
        return std::unexpected(std::move(step.error()));
    return {};
}

}
