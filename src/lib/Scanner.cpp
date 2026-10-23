#include "Scanner.h"

#include "ScanWalk.h"
#include "base/Hash.h"
#include "base/Log.h"
#include "base/Text.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace eb {
namespace {

using namespace scan_detail;
using Clock = std::chrono::steady_clock;

constexpr size_t kBatch = 200;
constexpr auto kFlushAfter = std::chrono::milliseconds(250);
constexpr size_t kLongPath = 240;
constexpr int kHidden = 3; // removed from the library by the user; kept so a rescan does not bring it back

struct Row {
    Id id{};
    int64_t size = 0;
    int64_t mtime = 0;
    int status = 0;
    bool seen = false;
};

enum class Kind : unsigned char { Insert, Touch, Move, Status, Redo, Retire };

struct Op {
    Kind kind = Kind::Touch;
    std::string id, id2, path, folder, title, author, title_nd, author_nd, lang, fmt;
    int64_t size = 0, mtime = 0, added = 0;
    int status = 0;
};

struct Resolved {
    std::string title, author, title_nd, author_nd, lang, fmt;
    int status = 0;
};

template <class... A>
Result<void> bind_all(Stmt& st, const A&... a)
{
    int i = 0;
    Result<void> res;
    auto one = [&](const auto& v) -> bool {
        if constexpr (std::is_same_v<std::decay_t<decltype(v)>, std::string>)
            res = st.bind_text(++i, v);
        else
            res = st.bind_int(++i, static_cast<int64_t>(v));
        return res.has_value();
    };
    (void)(one(a) && ...);
    return res;
}

Result<void> run_stmt(Stmt& st)
{
    auto s = st.step();
    auto r = st.reset();
    if (!s)
        return std::unexpected(std::move(s.error()));
    return r;
}

std::string folder_of(const std::string& path8)
{
    const auto cut = path8.rfind('\\');
    if (cut == std::string::npos)
        return path8;
    std::string f = path8.substr(0, cut);
    if (f.size() == 2 && f[1] == ':')
        f.push_back('\\');
    return f;
}

std::filesystem::path open_path(const std::string& path8)
{
    const std::wstring w = wide(path8);
    return std::filesystem::path(w.size() >= kLongPath ? prefixed(w) : w);
}

bool on_disk(const std::string& path8)
{
    return GetFileAttributesW(prefixed(wide(path8)).c_str()) != INVALID_FILE_ATTRIBUTES;
}

Result<Id> hash_id(const std::filesystem::path& p)
{
    auto h = book_id(p);
    if (!h)
        return std::unexpected(std::move(h.error()));
    if (auto id = id_from_hex(*h))
        return *id;
    return fail(ErrorCode::Internal, L"bad book id");
}

class Pass {
public:
    Pass(Db& db, const MetaFn& meta, CrashLoopGuard& guard)
        : db_(db), meta_(meta), guard_(guard), suspect_(guard.previous_suspect()) {}

    Result<void> prepare()
    {
        constexpr std::array<const char*, 8> sqls = {
            "INSERT INTO books(id,path,folder,title,author,title_nd,author_nd,lang,fmt,size,mtime,added_at,status)"
            " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13)",
            "UPDATE books SET size=?2,mtime=?3,status=?4 WHERE id=?1",
            "UPDATE books SET path=?2,folder=?3,size=?4,mtime=?5,status=?6 WHERE id=?1",
            "UPDATE books SET status=?2 WHERE id=?1",
            "UPDATE books SET id=?2,title=?3,author=?4,title_nd=?5,author_nd=?6,lang=?7,fmt=?8,size=?9,mtime=?10,status=?11"
            " WHERE id=?1",
            "UPDATE books SET path=?2,status=1 WHERE id=?1",
            "UPDATE progress SET book_id=?2 WHERE book_id=?1",
            "UPDATE bookmarks SET book_id=?2 WHERE book_id=?1"};
        const std::array<Stmt*, 8> dst = {&ins_, &touch_, &move_, &status_, &redo_, &retire_, &kids_p_, &kids_b_};
        for (size_t i = 0; i < sqls.size(); ++i) {
            auto st = db_.prepare(sqls[i]);
            if (!st)
                return std::unexpected(std::move(st.error()));
            *dst[i] = std::move(*st);
        }
        return load();
    }

    Result<ScanStats> run(const WalkResult& walk, const std::atomic<bool>& cancel,
        const std::function<void(int, int)>& progress)
    {
        for (const auto& e : walk.files) {
            if (const auto it = rows_.find(key_of(wide(e.path8))); it != rows_.end())
                it->second.seen = true;
        }
        const int total = static_cast<int>(walk.files.size());
        int done = 0;
        bool full = walk.complete;
        for (const auto& e : walk.files) {
            if (cancel.load(std::memory_order_relaxed)) {
                full = false;
                break;
            }
            if (auto r = process(e); !r)
                return std::unexpected(std::move(r.error()));
            if (progress)
                progress(++done, total);
        }
        if (full) {
            if (auto r = mark_missing(walk); !r)
                return std::unexpected(std::move(r.error()));
        }
        if (auto r = flush(); !r)
            return std::unexpected(std::move(r.error()));
        return stats_;
    }

private:
    Result<void> load()
    {
        auto st = db_.prepare("SELECT id,path,size,mtime,status FROM books");
        if (!st)
            return std::unexpected(std::move(st.error()));
        for (;;) {
            auto s = st->step();
            if (!s)
                return std::unexpected(std::move(s.error()));
            if (*s == Step::Done)
                return {};
            const auto id = id_from_hex(st->column_text(0));
            if (!id)
                continue;
            const auto [it, fresh_key] = rows_.try_emplace(key_of(wide(st->column_text(1))));
            if (!fresh_key)
                continue;
            Row& row = it->second;
            row = Row{*id, st->column_int(2), st->column_int(3), static_cast<int>(st->column_int(4)), false};
            by_id_[*id] = &row;
        }
    }

    Result<void> queue(Op op)
    {
        if (ops_.empty())
            first_ = Clock::now();
        ops_.push_back(std::move(op));
        if (ops_.size() >= kBatch || Clock::now() - first_ >= kFlushAfter)
            return flush();
        return {};
    }

    Result<void> flush()
    {
        if (ops_.empty())
            return {};
        auto tx = Tx::begin(db_);
        if (!tx)
            return std::unexpected(std::move(tx.error()));
        for (const Op& op : ops_) {
            if (auto r = apply(op); !r)
                return r;
        }
        ops_.clear();
        return tx->commit();
    }

    Result<void> apply(const Op& o)
    {
        switch (o.kind) {
        case Kind::Insert:
            if (auto b = bind_all(ins_, o.id, o.path, o.folder, o.title, o.author, o.title_nd, o.author_nd, o.lang, o.fmt,
                    o.size, o.mtime, o.added, o.status);
                !b)
                return b;
            return run_stmt(ins_);
        case Kind::Touch:
            if (auto b = bind_all(touch_, o.id, o.size, o.mtime, o.status); !b)
                return b;
            return run_stmt(touch_);
        case Kind::Move:
            if (auto b = bind_all(move_, o.id, o.path, o.folder, o.size, o.mtime, o.status); !b)
                return b;
            return run_stmt(move_);
        case Kind::Status:
            if (auto b = bind_all(status_, o.id, o.status); !b)
                return b;
            return run_stmt(status_);
        case Kind::Retire:
            if (auto b = bind_all(retire_, o.id, o.path); !b)
                return b;
            return run_stmt(retire_);
        case Kind::Redo:
            return apply_redo(o);
        }
        return {};
    }

    Result<void> apply_redo(const Op& o)
    {
        if (o.id != o.id2) {
            if (auto d = db_.exec("PRAGMA defer_foreign_keys=ON"); !d)
                return d;
            for (Stmt* k : {&kids_p_, &kids_b_}) {
                if (auto b = bind_all(*k, o.id, o.id2); !b)
                    return b;
                if (auto r = run_stmt(*k); !r)
                    return r;
            }
        }
        if (auto b = bind_all(redo_, o.id, o.id2, o.title, o.author, o.title_nd, o.author_nd, o.lang, o.fmt, o.size, o.mtime,
                o.status);
            !b)
            return b;
        return run_stmt(redo_);
    }

    Resolved resolve(const std::filesystem::path& p, const std::string& hex, std::wstring_view stem)
    {
        Resolved r;
        BookMeta m;
        if (suspect_ && *suspect_ == hex) {
            r.status = 2;
        } else {
            try {
                const CrashLoopGuard::Scope scope(guard_, "scan", hex);
                auto res = meta_(p);
                if (res)
                    m = std::move(*res);
                else
                    r.status = 2;
            } catch (...) {
                r.status = 2;
            }
        }
        if (m.title.empty())
            m.title = stem;
        if (m.fmt.empty())
            m.fmt = ext_of(p.filename().wstring());
        r.title = u8(m.title);
        r.author = u8(m.author);
        r.title_nd = u8(fold_for_search(m.title));
        r.author_nd = u8(fold_for_search(m.author));
        r.lang = u8(m.lang);
        r.fmt = u8(m.fmt);
        return r;
    }

    Op full_op(Kind kind, const Entry& e, const std::string& hex, const std::string& hex2)
    {
        const auto p = open_path(e.path8);
        const Resolved r = resolve(p, hex2, p.stem().wstring());
        Op op;
        op.kind = kind;
        op.id = hex;
        op.id2 = hex2;
        op.path = e.path8;
        op.folder = folder_of(e.path8);
        op.title = r.title;
        op.author = r.author;
        op.title_nd = r.title_nd;
        op.author_nd = r.author_nd;
        op.lang = r.lang;
        op.fmt = r.fmt;
        op.size = e.size;
        op.mtime = e.mtime;
        op.status = r.status;
        return op;
    }

    Result<void> process(const Entry& e)
    {
        const std::string key = key_of(wide(e.path8));
        if (auto it = rows_.find(key); it != rows_.end()) {
            Row& row = it->second;
            if (row.size == e.size && row.mtime == e.mtime)
                return unchanged(row);
            return changed(e, row);
        }
        return fresh(e, key);
    }

    Result<void> unchanged(Row& row)
    {
        ++stats_.unchanged;
        if (row.status != 1)
            return {};
        row.status = 0;
        Op op;
        op.kind = Kind::Status;
        op.id = id_to_hex(row.id);
        return queue(std::move(op));
    }

    Result<void> touch(Row& row, int status)
    {
        Op op;
        op.kind = Kind::Touch;
        op.id = id_to_hex(row.id);
        op.size = row.size;
        op.mtime = row.mtime;
        op.status = status;
        row.status = status;
        return queue(std::move(op));
    }

    Result<void> changed(const Entry& e, Row& row)
    {
        if (row.status == kHidden) {
            row.size = e.size;
            row.mtime = e.mtime;
            ++stats_.unchanged;
            return touch(row, kHidden);
        }
        auto id = hash_id(open_path(e.path8));
        if (!id) {
            ++stats_.bad;
            return {};
        }
        row.size = e.size;
        row.mtime = e.mtime;
        if (*id == row.id && row.status != 2) {
            ++stats_.updated;
            return touch(row, 0);
        }
        if (*id == row.id)
            return redo(e, row, *id);
        if (const auto o = by_id_.find(*id); o != by_id_.end())
            return duplicate_content(e, row, *o->second, *id);
        return redo(e, row, *id);
    }

    Result<void> redo(const Entry& e, Row& row, const Id& id)
    {
        Op op = full_op(Kind::Redo, e, id_to_hex(row.id), id_to_hex(id));
        by_id_.erase(row.id);
        row.id = id;
        row.size = e.size;
        row.mtime = e.mtime;
        row.status = op.status;
        by_id_[id] = &row;
        ++stats_.updated;
        return queue(std::move(op));
    }

    Result<void> duplicate_content(const Entry& e, Row& row, Row& other, const Id& id)
    {
        auto gone = vanished_path(other);
        if (!gone)
            return std::unexpected(std::move(gone.error()));
        if (!*gone) {
            ++stats_.bad;
            return touch(row, 2);
        }
        if (auto r = retire(e, row); !r)
            return r;
        return relocate(e, other, id, **gone);
    }

    Result<void> retire(const Entry& e, Row& row)
    {
        Op op;
        op.kind = Kind::Retire;
        op.id = id_to_hex(row.id);
        op.path = e.path8 + '\x1f' + op.id;
        auto node = rows_.extract(key_of(wide(e.path8)));
        row.status = 1;
        node.key() = key_of(wide(op.path));
        rows_.insert(std::move(node));
        return queue(std::move(op));
    }

    // nullopt: the other copy still exists (or was seen). Otherwise its stored path.
    Result<std::optional<std::string>> vanished_path(const Row& other)
    {
        if (other.seen)
            return std::optional<std::string>();
        if (auto f = flush(); !f)
            return std::unexpected(std::move(f.error()));
        auto st = db_.prepare("SELECT path FROM books WHERE id=?1");
        if (!st)
            return std::unexpected(std::move(st.error()));
        if (auto b = bind_all(*st, id_to_hex(other.id)); !b)
            return std::unexpected(std::move(b.error()));
        auto s = st->step();
        if (!s)
            return std::unexpected(std::move(s.error()));
        if (*s == Step::Done)
            return fail(ErrorCode::Internal, L"book row vanished");
        std::string path(st->column_text(0));
        if (on_disk(path))
            return std::optional<std::string>();
        return std::optional<std::string>(std::move(path));
    }

    Result<void> fresh(const Entry& e, const std::string& key)
    {
        auto id = hash_id(open_path(e.path8));
        if (!id) {
            ++stats_.bad;
            return {};
        }
        if (const auto o = by_id_.find(*id); o != by_id_.end()) {
            auto gone = vanished_path(*o->second);
            if (!gone)
                return std::unexpected(std::move(gone.error()));
            if (!*gone) {
                ++stats_.unchanged;
                return {};
            }
            return relocate(e, *o->second, *id, **gone);
        }
        return insert(e, key, *id);
    }

    Result<void> relocate(const Entry& e, Row& other, const Id& id, const std::string& old_path)
    {
        auto node = rows_.extract(key_of(wide(old_path)));
        if (node.empty())
            return fail(ErrorCode::Internal, L"relocate: row not found");
        other.size = e.size;
        other.mtime = e.mtime;
        other.seen = true;
        if (other.status != 2 && other.status != kHidden)
            other.status = 0;
        node.key() = key_of(wide(e.path8));
        rows_.insert(std::move(node));
        Op op;
        op.kind = Kind::Move;
        op.id = id_to_hex(id);
        op.path = e.path8;
        op.folder = folder_of(e.path8);
        op.size = e.size;
        op.mtime = e.mtime;
        op.status = other.status;
        ++stats_.moved;
        return queue(std::move(op));
    }

    Result<void> insert(const Entry& e, const std::string& key, const Id& id)
    {
        Op op = full_op(Kind::Insert, e, id_to_hex(id), id_to_hex(id));
        op.added = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        Row& row = rows_[key];
        row = Row{id, e.size, e.mtime, op.status, true};
        by_id_[id] = &row;
        ++(op.status == 2 ? stats_.bad : stats_.added);
        return queue(std::move(op));
    }

    static bool starts_with_any(const std::string& key, const std::vector<std::string>& prefixes)
    {
        return std::ranges::any_of(prefixes, [&](const std::string& p) { return key.starts_with(p); });
    }

    Result<void> mark_missing(const WalkResult& walk)
    {
        for (auto& [key, row] : rows_) {
            if (row.status != 0 || row.seen || !starts_with_any(key, walk.roots) || starts_with_any(key, walk.holes))
                continue;
            row.status = 1;
            ++stats_.missing;
            Op op;
            op.kind = Kind::Status;
            op.id = id_to_hex(row.id);
            op.status = 1;
            if (auto r = queue(std::move(op)); !r)
                return r;
        }
        return {};
    }

    Db& db_;
    const MetaFn& meta_;
    CrashLoopGuard& guard_;
    std::optional<std::string> suspect_;
    Stmt ins_{nullptr}, touch_{nullptr}, move_{nullptr}, status_{nullptr}, redo_{nullptr}, retire_{nullptr},
        kids_p_{nullptr}, kids_b_{nullptr};
    std::unordered_map<std::string, Row> rows_;
    std::unordered_map<Id, Row*, IdHash> by_id_;
    std::vector<Op> ops_;
    Clock::time_point first_;
    ScanStats stats_;
};

}

Result<ScanStats> scan_library(Db& db, std::span<const std::filesystem::path> roots, const MetaFn& meta, CrashLoopGuard& guard,
    const std::atomic<bool>& cancel, const std::function<void(int, int)>& progress,
    std::span<const std::wstring> exclude_dirs) noexcept
{
    try {
        const WalkResult walk = walk_roots(roots, exclude_dirs, cancel);
        Pass pass(db, meta, guard);
        if (auto r = pass.prepare(); !r)
            return std::unexpected(std::move(r.error()));
        return pass.run(walk, cancel, progress);
    } catch (...) {
        return fail(ErrorCode::Internal, L"scan failed");
    }
}

}
