#include "Scanner.h"

#include "base/Hash.h"
#include "base/Log.h"
#include "base/Text.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace eb {
namespace {

constexpr int kBatch = 200;
constexpr size_t kLongPath = 240;
constexpr std::array<std::wstring_view, 9> kExts = {
    L"epub", L"pdf", L"mobi", L"azw3", L"azw", L"prc", L"fb2", L"cbz", L"txt"};

struct Entry {
    std::filesystem::path path;
    std::string path8;
    int64_t size = 0;
    int64_t mtime = 0;
};

struct Row {
    std::string id;
    int64_t size = 0;
    int64_t mtime = 0;
    int status = 0;
};

std::wstring lower(std::wstring s)
{
    std::ranges::transform(s, s.begin(), [](wchar_t c) { return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + 32) : c; });
    return s;
}

std::wstring ext_of(const std::wstring& name)
{
    const auto dot = name.rfind(L'.');
    return dot == std::wstring::npos ? std::wstring() : lower(name.substr(dot + 1));
}

bool wanted_ext(const std::wstring& name)
{
    return std::ranges::find(kExts, ext_of(name)) != kExts.end();
}

std::wstring prefixed(const std::wstring& p)
{
    if (p.starts_with(L"\\\\?\\"))
        return p;
    if (p.starts_with(L"\\\\"))
        return L"\\\\?\\UNC\\" + p.substr(2);
    return L"\\\\?\\" + p;
}

std::string u8(std::wstring_view w)
{
    auto r = wide_to_utf8(w);
    return r ? std::move(*r) : std::string();
}

int64_t ticks(const FILETIME& ft)
{
    return static_cast<int64_t>((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
}

bool excluded(const std::wstring& name, std::span<const std::wstring> ex)
{
    return std::ranges::any_of(ex, [&](const std::wstring& e) { return lower(e) == lower(name); });
}

class Walker {
public:
    Walker(std::span<const std::wstring> ex, const std::atomic<bool>& cancel, std::vector<Entry>& out)
        : ex_(ex), cancel_(cancel), out_(out) {}

    // Returns true when the walk saw everything.
    bool run(std::span<const std::filesystem::path> roots)
    {
        for (const auto& r : roots) {
            std::error_code ec;
            std::wstring dir = std::filesystem::absolute(r, ec).lexically_normal().wstring();
            while (dir.size() > 1 && dir.back() == L'\\')
                dir.pop_back();
            const DWORD a = ec ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(prefixed(dir).c_str());
            if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT)) {
                log::info("scan: root skipped");
                continue;
            }
            if (!walk(dir))
                return false;
        }
        return true;
    }

private:
    bool walk(const std::wstring& root)
    {
        std::vector<std::wstring> stack{root};
        while (!stack.empty()) {
            if (cancel_.load(std::memory_order_relaxed))
                return false;
            const std::wstring dir = std::move(stack.back());
            stack.pop_back();
            if (!list(dir, stack))
                return false;
        }
        return true;
    }

    bool list(const std::wstring& dir, std::vector<std::wstring>& stack)
    {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileExW((prefixed(dir) + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
            FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) {
            log::info("scan: directory unreadable");
            return true;
        }
        bool ok = true;
        do {
            ok = take(dir, fd, stack);
        } while (ok && FindNextFileW(h, &fd));
        FindClose(h);
        return ok;
    }

    bool take(const std::wstring& dir, const WIN32_FIND_DATAW& fd, std::vector<std::wstring>& stack)
    {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..")
            return true;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_REPARSE_POINT))
            return true;
        std::wstring full = dir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!excluded(name, ex_))
                stack.push_back(std::move(full));
            return true;
        }
        if (!wanted_ext(name))
            return true;
        if (out_.size() >= kMaxScanFiles) {
            log::error("scan: file cap reached");
            return false;
        }
        std::string p8 = u8(full);
        if (p8.empty())
            return true;
        const int64_t size = static_cast<int64_t>((static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow);
        out_.push_back({std::filesystem::path(full.size() >= kLongPath ? prefixed(full) : full), std::move(p8), size,
            ticks(fd.ftLastWriteTime)});
        return true;
    }

    std::span<const std::wstring> ex_;
    const std::atomic<bool>& cancel_;
    std::vector<Entry>& out_;
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

std::string folder_of(const Entry& e)
{
    const std::wstring p = e.path.wstring();
    const auto cut = p.rfind(L'\\');
    return u8(cut == std::wstring::npos ? p : p.substr(0, cut));
}

class Pass {
public:
    Pass(Db& db, const MetaFn& meta, CrashLoopGuard& guard)
        : db_(db), meta_(meta), guard_(guard), suspect_(guard.previous_suspect()) {}

    Result<void> prepare()
    {
        constexpr std::array<const char*, 5> sqls = {
            "INSERT INTO books(id,path,folder,title,author,title_nd,author_nd,lang,fmt,size,mtime,added_at,status)"
            " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13)",
            "UPDATE books SET size=?2,mtime=?3,status=?4 WHERE id=?1",
            "UPDATE books SET path=?2,folder=?3,size=?4,mtime=?5,status=?6 WHERE id=?1",
            "UPDATE books SET status=?2 WHERE id=?1",
            "UPDATE books SET id=?2,title=?3,author=?4,title_nd=?5,author_nd=?6,lang=?7,fmt=?8,size=?9,mtime=?10,status=?11"
            " WHERE id=?1"};
        const std::array<Stmt*, 5> dst = {&ins_, &touch_, &move_, &status_, &redo_};
        for (size_t i = 0; i < sqls.size(); ++i) {
            auto st = db_.prepare(sqls[i]);
            if (!st)
                return std::unexpected(std::move(st.error()));
            *dst[i] = std::move(*st);
        }
        return load();
    }

    Result<ScanStats> run(const std::vector<Entry>& files, bool complete, const std::atomic<bool>& cancel,
        const std::function<void(int, int)>& progress)
    {
        for (const auto& e : files)
            seen_.insert(e.path8);
        const int total = static_cast<int>(files.size());
        int done = 0;
        bool full = complete;
        for (const auto& e : files) {
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
            if (auto r = mark_missing(); !r)
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
            std::string path(st->column_text(1));
            Row row{std::string(st->column_text(0)), st->column_int(2), st->column_int(3), static_cast<int>(st->column_int(4))};
            byId_[row.id] = path;
            rows_[std::move(path)] = std::move(row);
        }
    }

    Result<void> write_begin()
    {
        if (tx_)
            return {};
        auto t = Tx::begin(db_);
        if (!t)
            return std::unexpected(std::move(t.error()));
        tx_.emplace(std::move(*t));
        return {};
    }

    Result<void> flush()
    {
        pending_ = 0;
        if (!tx_)
            return {};
        auto r = tx_->commit();
        tx_.reset();
        return r;
    }

    Result<void> after_write()
    {
        return ++pending_ >= kBatch ? flush() : Result<void>{};
    }

    Resolved resolve(const Entry& e, const std::string& id)
    {
        Resolved r;
        BookMeta m;
        if (suspect_ && *suspect_ == id) {
            r.status = 2;
        } else {
            if (!guard_.begin("scan", id))
                log::error("scan: crash marker write failed");
            auto res = meta_(e.path);
            guard_.end();
            if (res)
                m = std::move(*res);
            else
                r.status = 2;
        }
        if (m.title.empty())
            m.title = e.path.stem().wstring();
        if (m.fmt.empty())
            m.fmt = ext_of(e.path.filename().wstring());
        r.title = u8(m.title);
        r.author = u8(m.author);
        r.title_nd = u8(fold_for_search(m.title));
        r.author_nd = u8(fold_for_search(m.author));
        r.lang = u8(m.lang);
        r.fmt = u8(m.fmt);
        return r;
    }

    Result<void> process(const Entry& e)
    {
        if (auto it = rows_.find(e.path8); it != rows_.end()) {
            Row& row = it->second;
            if (row.size == e.size && row.mtime == e.mtime)
                return unchanged(row);
            return changed(e, row);
        }
        return fresh(e);
    }

    Result<void> unchanged(Row& row)
    {
        ++stats_.unchanged;
        if (row.status != 1)
            return {};
        row.status = 0;
        return set_status(row.id, 0);
    }

    Result<void> exec_write(Stmt& st)
    {
        if (auto s = run_stmt(st); !s)
            return s;
        return after_write();
    }

    Result<void> set_status(const std::string& id, int status)
    {
        if (auto r = write_begin(); !r)
            return r;
        if (auto r = bind_all(status_, id, status); !r)
            return r;
        return exec_write(status_);
    }

    Result<void> changed(const Entry& e, Row& row)
    {
        auto id = book_id(e.path);
        if (!id) {
            ++stats_.bad;
            return {};
        }
        if (*id != row.id && byId_.contains(*id)) {
            ++stats_.unchanged;
            return {};
        }
        if (*id == row.id && row.status != 2) {
            row.size = e.size;
            row.mtime = e.mtime;
            row.status = 0;
            ++stats_.updated;
            if (auto r = write_begin(); !r)
                return r;
            if (auto b = bind_all(touch_, row.id, row.size, row.mtime, row.status); !b)
                return b;
            return exec_write(touch_);
        }
        return redo(e, row, *id);
    }

    Result<void> redo(const Entry& e, Row& row, const std::string& id)
    {
        const Resolved r = resolve(e, id);
        if (auto t = write_begin(); !t)
            return t;
        if (id != row.id) {
            if (auto d = db_.exec("PRAGMA defer_foreign_keys=ON"); !d)
                return d;
            if (auto m = move_children(row.id, id); !m)
                return m;
        }
        if (auto b = bind_all(redo_, row.id, id, r.title, r.author, r.title_nd, r.author_nd, r.lang, r.fmt, e.size, e.mtime,
                r.status);
            !b)
            return b;
        if (auto s = exec_write(redo_); !s)
            return s;
        byId_.erase(row.id);
        row = Row{id, e.size, e.mtime, r.status};
        byId_[id] = e.path8;
        ++stats_.updated;
        return {};
    }

    Result<void> move_children(const std::string& from, const std::string& to)
    {
        for (const char* sql : {"UPDATE progress SET book_id=?2 WHERE book_id=?1",
                 "UPDATE bookmarks SET book_id=?2 WHERE book_id=?1"}) {
            auto st = db_.prepare(sql);
            if (!st)
                return std::unexpected(std::move(st.error()));
            if (auto b = bind_all(*st, from, to); !b)
                return b;
            if (auto s = st->step(); !s)
                return std::unexpected(std::move(s.error()));
        }
        return {};
    }

    Result<void> fresh(const Entry& e)
    {
        auto id = book_id(e.path);
        if (!id) {
            ++stats_.bad;
            return {};
        }
        if (auto o = byId_.find(*id); o != byId_.end()) {
            if (seen_.contains(o->second)) {
                ++stats_.unchanged;
                return {};
            }
            return relocate(e, *id, o->second);
        }
        return insert(e, *id);
    }

    Result<void> relocate(const Entry& e, const std::string& id, const std::string& old_path)
    {
        auto node = rows_.extract(old_path);
        Row& row = node.mapped();
        row.size = e.size;
        row.mtime = e.mtime;
        if (row.status != 2)
            row.status = 0;
        const int status = row.status;
        node.key() = e.path8;
        rows_.insert(std::move(node));
        byId_[id] = e.path8;
        if (auto t = write_begin(); !t)
            return t;
        if (auto b = bind_all(move_, id, e.path8, folder_of(e), e.size, e.mtime, status); !b)
            return b;
        ++stats_.moved;
        return exec_write(move_);
    }

    Result<void> insert(const Entry& e, const std::string& id)
    {
        const Resolved r = resolve(e, id);
        if (auto t = write_begin(); !t)
            return t;
        const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (auto b = bind_all(ins_, id, e.path8, folder_of(e), r.title, r.author, r.title_nd, r.author_nd, r.lang, r.fmt,
                e.size, e.mtime, now, r.status);
            !b)
            return b;
        rows_[e.path8] = Row{id, e.size, e.mtime, r.status};
        byId_[id] = e.path8;
        ++(r.status == 2 ? stats_.bad : stats_.added);
        return exec_write(ins_);
    }

    Result<void> mark_missing()
    {
        for (auto& [path, row] : rows_) {
            if (row.status != 0 || seen_.contains(path))
                continue;
            row.status = 1;
            ++stats_.missing;
            if (auto r = set_status(row.id, 1); !r)
                return r;
        }
        return {};
    }

    Db& db_;
    const MetaFn& meta_;
    CrashLoopGuard& guard_;
    std::optional<std::string> suspect_;
    Stmt ins_{nullptr}, touch_{nullptr}, move_{nullptr}, status_{nullptr}, redo_{nullptr};
    std::unordered_map<std::string, Row> rows_;
    std::unordered_map<std::string, std::string> byId_;
    std::unordered_set<std::string> seen_;
    std::optional<Tx> tx_;
    int pending_ = 0;
    ScanStats stats_;
};

}

Result<ScanStats> scan_library(Db& db, std::span<const std::filesystem::path> roots, const MetaFn& meta, CrashLoopGuard& guard,
    const std::atomic<bool>& cancel, const std::function<void(int, int)>& progress,
    std::span<const std::wstring> exclude_dirs) noexcept
{
    try {
        std::vector<Entry> files;
        const bool complete = Walker(exclude_dirs, cancel, files).run(roots);
        Pass pass(db, meta, guard);
        if (auto r = pass.prepare(); !r)
            return std::unexpected(std::move(r.error()));
        return pass.run(files, complete, cancel, progress);
    } catch (...) {
        return fail(ErrorCode::Internal, L"scan failed");
    }
}

}
