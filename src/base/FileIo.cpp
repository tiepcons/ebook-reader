#include "FileIo.h"

#include "WinError.h"

#include <algorithm>
#include <new>

namespace eb {

FileReader::FileReader(const std::filesystem::path& p) noexcept
    : h_(CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
          FILE_ATTRIBUTE_NORMAL, nullptr))
{
}

bool FileReader::size(uint64_t& out) const noexcept
{
    LARGE_INTEGER n{};
    if (!GetFileSizeEx(h_.get(), &n) || n.QuadPart < 0)
        return false;
    out = static_cast<uint64_t>(n.QuadPart);
    return true;
}

bool FileReader::read(void* buf, size_t want, size_t& got) noexcept
{
    got = 0;
    constexpr size_t kMaxChunk = 1u << 30;
    DWORD n = 0;
    if (!ReadFile(h_.get(), buf, static_cast<DWORD>(std::min(want, kMaxChunk)), &n, nullptr))
        return false;
    got = n;
    return true;
}

namespace {

// Fills buf until full or EOF; false on an I/O error.
bool fill(FileReader& f, std::string& buf, size_t& total) noexcept
{
    total = 0;
    while (total < buf.size()) {
        size_t got = 0;
        if (!f.read(buf.data() + total, buf.size() - total, got))
            return false;
        if (got == 0)
            break;
        total += got;
    }
    return true;
}

}

std::string read_head(const std::filesystem::path& p, size_t max)
{
    FileReader f(p);
    std::string s;
    if (!f.ok())
        return s;
    s.assign(max, '\0');
    size_t total = 0;
    if (!fill(f, s, total))
        total = 0;
    s.resize(total);
    return s;
}

Result<std::string> read_all(const std::filesystem::path& p, size_t max)
{
    try {
        FileReader f(p);
        if (!f.ok())
            return fail_win32(GetLastError(), L"open");
        uint64_t size = 0;
        if (!f.size(size))
            return fail_win32(GetLastError(), L"size");
        if (size > max)
            return fail(ErrorCode::Limit, L"file too large");
        std::string s(static_cast<size_t>(size), '\0');
        size_t total = 0;
        if (!fill(f, s, total))
            return fail(ErrorCode::Io, L"read");
        s.resize(total);
        return s;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::NoMemory);
    }
}

bool write_file(const std::filesystem::path& p, std::string_view data) noexcept
{
    const UniqueHandle h(CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!h)
        return false;
    while (!data.empty()) {
        DWORD n = 0;
        const auto want = static_cast<DWORD>(std::min<size_t>(data.size(), 1u << 30));
        if (!WriteFile(h.get(), data.data(), want, &n, nullptr) || n == 0)
            return false;
        data.remove_prefix(n);
    }
    return true;
}

}
