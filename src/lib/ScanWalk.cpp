#include "ScanWalk.h"

#include "Scanner.h"
#include "base/Handle.h"
#include "base/Log.h"
#include "base/Text.h"

#include <windows.h>

#include <algorithm>
#include <array>

namespace eb {

namespace scan_path {

std::wstring strip_long_prefix(std::wstring_view p)
{
    if (p.starts_with(L"\\\\?\\UNC\\"))
        return L"\\\\" + std::wstring(p.substr(8));
    if (p.starts_with(L"\\\\?\\"))
        return std::wstring(p.substr(4));
    return std::wstring(p);
}

std::wstring join(std::wstring_view dir, std::wstring_view name)
{
    std::wstring r(dir);
    if (r.empty() || r.back() != L'\\')
        r.push_back(L'\\');
    r.append(name);
    return r;
}

std::wstring pattern(std::wstring_view dir)
{
    return join(dir, L"*");
}

}

namespace scan_detail {
namespace {

constexpr std::array<std::wstring_view, 9> kExts = {
    L"epub", L"pdf", L"mobi", L"azw3", L"azw", L"prc", L"fb2", L"cbz", L"txt"};

bool same_name(const std::wstring& a, const std::wstring& b)
{
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

int64_t ticks(const FILETIME& ft)
{
    return static_cast<int64_t>((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
}

// Opens without following a reparse point; returns the real path, or empty if unusable.
std::wstring canonical_root(const std::filesystem::path& r)
{
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(r, ec).lexically_normal();
    if (ec)
        return {};
    const UniqueHandle h(CreateFileW(prefixed(abs.make_preferred().wstring()).c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    if (!h || !GetFileInformationByHandle(h.get(), &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return {};
    std::wstring buf(MAX_PATH, L'\0');
    DWORD n = GetFinalPathNameByHandleW(h.get(), buf.data(), static_cast<DWORD>(buf.size()), FILE_NAME_NORMALIZED);
    if (n >= buf.size()) {
        buf.resize(n);
        n = GetFinalPathNameByHandleW(h.get(), buf.data(), static_cast<DWORD>(buf.size()), FILE_NAME_NORMALIZED);
    }
    if (n == 0 || n >= buf.size())
        return {};
    buf.resize(n);
    return scan_path::strip_long_prefix(buf);
}

class Walker {
public:
    Walker(std::span<const std::wstring> ex, const std::atomic<bool>& cancel, WalkResult& out)
        : ex_(ex), cancel_(cancel), out_(out) {}

    void run(std::span<const std::filesystem::path> roots)
    {
        for (const auto& r : roots) {
            const std::wstring dir = canonical_root(r);
            if (dir.empty()) {
                log::info("scan: root skipped");
                continue;
            }
            out_.roots.push_back(dir_prefix(dir));
            if (!walk(dir)) {
                out_.complete = false;
                return;
            }
        }
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
        HANDLE h = FindFirstFileExW(prefixed(scan_path::pattern(dir)).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
            nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) {
            out_.holes.push_back(dir_prefix(dir));
            return true;
        }
        bool ok = true;
        do {
            ok = take(dir, fd, stack);
        } while (ok && FindNextFileW(h, &fd));
        if (ok && GetLastError() != ERROR_NO_MORE_FILES)
            out_.holes.push_back(dir_prefix(dir));
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
        std::wstring full = scan_path::join(dir, name);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (std::ranges::none_of(ex_, [&](const std::wstring& e) { return same_name(e, name); }))
                stack.push_back(std::move(full));
            return true;
        }
        if (std::ranges::find(kExts, ext_of(name)) == kExts.end())
            return true;
        if (out_.files.size() >= kMaxScanFiles) {
            log::error("scan: file cap reached");
            return false;
        }
        std::string p8 = u8(full);
        if (p8.empty())
            return true;
        const int64_t size = static_cast<int64_t>((static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow);
        out_.files.push_back({std::move(p8), size, ticks(fd.ftLastWriteTime)});
        return true;
    }

    std::span<const std::wstring> ex_;
    const std::atomic<bool>& cancel_;
    WalkResult& out_;
};

}

WalkResult walk_roots(std::span<const std::filesystem::path> roots, std::span<const std::wstring> exclude,
    const std::atomic<bool>& cancel)
{
    WalkResult out;
    Walker(exclude, cancel, out).run(roots);
    return out;
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

std::wstring wide(std::string_view s)
{
    auto r = utf8_to_wide(s);
    return r ? std::move(*r) : std::wstring();
}

std::wstring ext_of(std::wstring_view name)
{
    const auto dot = name.rfind(L'.');
    if (dot == std::wstring_view::npos)
        return {};
    std::wstring e(name.substr(dot + 1));
    std::ranges::transform(e, e.begin(), [](wchar_t c) { return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + 32) : c; });
    return e;
}

std::string key_of(std::wstring_view w)
{
    if (w.empty())
        return {};
    std::wstring up(w.size(), L'\0');
    const int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, w.data(), static_cast<int>(w.size()), up.data(),
        static_cast<int>(up.size()), nullptr, nullptr, 0);
    if (n <= 0)
        return u8(w);
    up.resize(static_cast<size_t>(n));
    return u8(up);
}

std::string dir_prefix(std::wstring_view dir)
{
    std::string k = key_of(dir);
    if (k.empty() || k.back() != '\\')
        k.push_back('\\');
    return k;
}


std::optional<Id> id_from_hex(std::string_view s)
{
    if (s.size() != 64)
        return std::nullopt;
    Id id{};
    for (size_t i = 0; i < 32; ++i) {
        int v = 0;
        for (const char c : s.substr(i * 2, 2)) {
            const int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
            if (d < 0)
                return std::nullopt;
            v = v * 16 + d;
        }
        id[i] = static_cast<uint8_t>(v);
    }
    return id;
}

std::string id_to_hex(const Id& id)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string s;
    s.reserve(64);
    for (const uint8_t b : id) {
        s.push_back(kDigits[b >> 4]);
        s.push_back(kDigits[b & 15]);
    }
    return s;
}

}
}
