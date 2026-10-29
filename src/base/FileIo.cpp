#include "FileIo.h"

#include "WinError.h"

#include <algorithm>
#include <new>
#include <vector>

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
bool fill(FileReader& f, void* buf, size_t cap, size_t& total) noexcept
{
    total = 0;
    while (total < cap) {
        size_t got = 0;
        if (!f.read(static_cast<char*>(buf) + total, cap - total, got))
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
    if (!fill(f, s.data(), s.size(), total))
        total = 0;
    s.resize(total);
    return s;
}

namespace {

template <class Buf>
Result<Buf> read_sized(const std::filesystem::path& p, size_t max)
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
        Buf b;
        b.resize(static_cast<size_t>(size));
        size_t total = 0;
        if (!fill(f, b.data(), b.size(), total))
            return fail(ErrorCode::Io, L"read");
        if (total != b.size())
            return fail(ErrorCode::Io, L"short read");
        return b;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::NoMemory);
    }
}

bool write_to(const std::filesystem::path& p, DWORD disposition, DWORD flags, std::string_view data) noexcept
{
    const UniqueHandle h(CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        disposition, FILE_ATTRIBUTE_NORMAL | flags, nullptr));
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

Result<std::string> read_all(const std::filesystem::path& p, size_t max)
{
    return read_sized<std::string>(p, max);
}

Result<std::vector<std::byte>> read_all_bytes(const std::filesystem::path& p, size_t max)
{
    return read_sized<std::vector<std::byte>>(p, max);
}

bool write_file(const std::filesystem::path& p, std::string_view data) noexcept
{
    return write_to(p, CREATE_ALWAYS, 0, data);
}

bool write_new(const std::filesystem::path& p, std::string_view data) noexcept
{
    return write_to(p, CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT, data);
}

}
