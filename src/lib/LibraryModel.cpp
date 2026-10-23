#include "LibraryModel.h"

#include "base/Text.h"

#include <windows.h>

#include <algorithm>
#include <map>

namespace eb {
namespace {

constexpr size_t kMaxTokens = 8;
constexpr size_t kMaxQueryChars = 256;

std::string escape_like(std::string_view s)
{
    std::string out;
    out.reserve(s.size() + 2);
    for (const char c : s) {
        if (c == '!' || c == '%' || c == '_')
            out.push_back('!');
        out.push_back(c);
    }
    return out;
}

std::vector<std::string> search_tokens(const std::wstring& text)
{
    std::vector<std::string> out;
    const std::wstring folded = fold_for_search(text.substr(0, kMaxQueryChars));
    size_t i = 0;
    while (i < folded.size() && out.size() < kMaxTokens) {
        while (i < folded.size() && folded[i] <= L' ')
            ++i;
        const size_t b = i;
        while (i < folded.size() && folded[i] > L' ')
            ++i;
        if (i > b) {
            if (auto u = wide_to_utf8(std::wstring_view(folded).substr(b, i - b)); u)
                out.push_back(std::move(*u));
        }
    }
    return out;
}

const char* order_by(SortBy s) noexcept
{
    switch (s) {
    case SortBy::Author:
        return " ORDER BY b.author_nd, b.title_nd, b.id";
    case SortBy::Added:
        return " ORDER BY b.added_at DESC, b.title_nd, b.id";
    case SortBy::LastOpened:
        return " ORDER BY b.last_opened DESC, b.title_nd, b.id";
    default:
        return " ORDER BY b.title_nd, b.author_nd, b.id";
    }
}

std::wstring to_wide(std::string_view s)
{
    return utf8_to_wide(s).value_or(std::wstring());
}

Result<void> bind_args(Stmt& st, const std::vector<std::string>& args)
{
    int i = 0;
    for (const auto& a : args) {
        if (auto r = st.bind_text(++i, a); !r)
            return r;
    }
    return {};
}

std::wstring lower(std::wstring s)
{
    if (!s.empty())
        CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

std::wstring trim_slash(std::wstring p)
{
    while (p.size() > 3 && p.back() == L'\\')
        p.pop_back();
    return p;
}

std::wstring leaf_name(const std::wstring& p)
{
    const size_t cut = p.find_last_of(L'\\');
    const std::wstring leaf = cut == std::wstring::npos ? p : p.substr(cut + 1);
    return leaf.empty() ? p : leaf;
}

bool is_under(const std::wstring& lower_folder, const std::wstring& lower_root)
{
    if (lower_folder.size() < lower_root.size() || lower_folder.compare(0, lower_root.size(), lower_root) != 0)
        return false;
    return lower_folder.size() == lower_root.size() || lower_root.back() == L'\\'
        || lower_folder[lower_root.size()] == L'\\';
}

struct TreeBuilder {
    std::vector<FolderNode> nodes;
    std::map<std::wstring, size_t> index; // lower-case prefix -> node

    size_t ensure(const std::wstring& prefix, std::wstring label, int parent)
    {
        const auto [it, fresh] = index.try_emplace(lower(prefix), nodes.size());
        if (fresh)
            nodes.push_back({std::move(label), prefix, parent});
        return it->second;
    }

    void add_folder(const std::wstring& folder, const std::wstring& root)
    {
        size_t cur = ensure(root, leaf_name(root), -1);
        size_t pos = root.size();
        while (pos < folder.size()) {
            if (folder[pos] == L'\\')
                ++pos;
            const size_t end = std::min(folder.find(L'\\', pos), folder.size());
            if (end == pos)
                break;
            cur = ensure(folder.substr(0, end), folder.substr(pos, end - pos), static_cast<int>(cur));
            pos = end;
        }
    }
};

std::vector<FolderNode> emit_sorted(const std::vector<FolderNode>& in)
{
    std::vector<std::vector<size_t>> kids(in.size() + 1); // slot in.size() holds the roots
    for (size_t i = 0; i < in.size(); ++i)
        kids[in[i].parent < 0 ? in.size() : static_cast<size_t>(in[i].parent)].push_back(i);
    for (auto& k : kids) {
        std::ranges::sort(k, [&](size_t a, size_t b) { return lower(in[a].label) > lower(in[b].label); });
    }
    std::vector<FolderNode> out;
    out.reserve(in.size());
    std::vector<std::pair<size_t, int>> todo; // source node, output index of its parent
    for (const size_t r : kids[in.size()])
        todo.emplace_back(r, -1);
    while (!todo.empty()) {
        const auto [node, parent] = todo.back();
        todo.pop_back();
        out.push_back({in[node].label, in[node].prefix, parent});
        for (const size_t k : kids[node])
            todo.emplace_back(k, static_cast<int>(out.size()) - 1);
    }
    return out;
}

Result<std::vector<std::string>> step_all_text(Db& db, const char* sql)
{
    auto st = db.prepare(sql);
    if (!st)
        return std::unexpected(std::move(st.error()));
    std::vector<std::string> out;
    for (;;) {
        auto step = st->step();
        if (!step)
            return std::unexpected(std::move(step.error()));
        if (*step == Step::Done)
            return out;
        out.emplace_back(st->column_text(0));
    }
}

}

Result<std::vector<BookRow>> query_books(Db& db, const Query& q) noexcept
{
    try {
        std::string sql = "SELECT b.id,b.title,b.author,b.fmt,COALESCE(p.percent,0),b.status FROM books b "
                          "LEFT JOIN progress p ON p.book_id=b.id WHERE 1";
        std::vector<std::string> args;
        if (q.folder) {
            auto f = wide_to_utf8(*q.folder);
            if (!f)
                return std::unexpected(std::move(f.error()));
            sql += " AND (b.folder=? OR b.folder LIKE ? ESCAPE '!')";
            args.push_back(*f);
            args.push_back(escape_like(*f) + (f->ends_with('\\') ? "%" : "\\%"));
        }
        for (const auto& t : search_tokens(q.text)) {
            sql += " AND (b.title_nd LIKE ? ESCAPE '!' OR b.author_nd LIKE ? ESCAPE '!')";
            args.push_back("%" + escape_like(t) + "%");
            args.push_back(args.back());
        }
        if (!q.fmt.empty()) {
            sql += " AND b.fmt=? COLLATE NOCASE";
            args.push_back(q.fmt);
        }
        if (q.reading_only)
            sql += " AND p.percent>0 AND p.percent<0.99";
        sql += order_by(q.sort);
        auto st = db.prepare(sql);
        if (!st)
            return std::unexpected(std::move(st.error()));
        if (auto r = bind_args(*st, args); !r)
            return std::unexpected(std::move(r.error()));
        std::vector<BookRow> rows;
        for (;;) {
            auto step = st->step();
            if (!step)
                return std::unexpected(std::move(step.error()));
            if (*step == Step::Done)
                return rows;
            rows.push_back({std::string(st->column_text(0)), to_wide(st->column_text(1)), to_wide(st->column_text(2)),
                std::string(st->column_text(3)), st->column_double(4), static_cast<int>(st->column_int(5))});
        }
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::NoMemory);
    } catch (...) {
        return fail(ErrorCode::Internal);
    }
}

Result<std::vector<std::wstring>> list_folders(Db& db) noexcept
{
    try {
        auto rows = step_all_text(db, "SELECT DISTINCT folder FROM books WHERE status<>1 ORDER BY folder");
        if (!rows)
            return std::unexpected(std::move(rows.error()));
        std::vector<std::wstring> out;
        out.reserve(rows->size());
        for (const auto& r : *rows)
            out.push_back(to_wide(r));
        return out;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::NoMemory);
    } catch (...) {
        return fail(ErrorCode::Internal);
    }
}

Result<std::wstring> book_path(Db& db, std::string_view id) noexcept
{
    try {
        auto st = db.prepare("SELECT path FROM books WHERE id=?1");
        if (!st)
            return std::unexpected(std::move(st.error()));
        if (auto r = st->bind_text(1, id); !r)
            return std::unexpected(std::move(r.error()));
        auto step = st->step();
        if (!step)
            return std::unexpected(std::move(step.error()));
        if (*step == Step::Done)
            return fail(ErrorCode::NotFound);
        std::string_view p = st->column_text(0);
        p = p.substr(0, p.find('\x1f'));
        return to_wide(p);
    } catch (...) {
        return fail(ErrorCode::Internal);
    }
}

Result<void> remove_book(Db& db, std::string_view id) noexcept
{
    try {
        auto st = db.prepare("DELETE FROM books WHERE id=?1");
        if (!st)
            return std::unexpected(std::move(st.error()));
        if (auto r = st->bind_text(1, id); !r)
            return r;
        auto step = st->step();
        if (!step)
            return std::unexpected(std::move(step.error()));
        return {};
    } catch (...) {
        return fail(ErrorCode::Internal);
    }
}

std::vector<FolderNode> build_folder_tree(std::span<const std::wstring> folders, std::span<const std::wstring> roots)
{
    TreeBuilder tb;
    std::vector<std::wstring> rs;
    for (const auto& r : roots) {
        if (!r.empty())
            rs.push_back(trim_slash(r));
    }
    for (const auto& r : rs)
        tb.ensure(r, leaf_name(r), -1);
    for (const auto& f : folders) {
        const std::wstring lf = lower(f);
        const std::wstring* best = nullptr;
        for (const auto& r : rs) {
            if (is_under(lf, lower(r)) && (best == nullptr || r.size() > best->size()))
                best = &r;
        }
        if (best != nullptr)
            tb.add_folder(f, *best);
        else
            tb.ensure(f, f, -1);
    }
    return emit_sorted(tb.nodes);
}

}
