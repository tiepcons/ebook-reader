#include "Hash.h"

#include "Handle.h"
#include "WinError.h"

#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <new>
#include <vector>

namespace eb {
namespace {

constexpr std::uint64_t kSlice = 64ULL * 1024;

Result<void> read_at(HANDLE h, std::uint64_t offset, std::uint8_t* dst, size_t n)
{
    LARGE_INTEGER pos{};
    pos.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN))
        return fail_win32(GetLastError(), L"SetFilePointerEx");
    DWORD got = 0;
    if (!ReadFile(h, dst, static_cast<DWORD>(n), &got, nullptr))
        return fail_win32(GetLastError(), L"ReadFile");
    if (got != n)
        return fail(ErrorCode::Io, L"short read");
    return {};
}

Result<std::vector<std::uint8_t>> sample(HANDLE h, std::uint64_t size)
{
    const size_t head = static_cast<size_t>(std::min(size, kSlice));
    const size_t tail = size > kSlice ? static_cast<size_t>(kSlice) : 0;
    std::vector<std::uint8_t> buf(8 + head + tail);
    for (size_t i = 0; i < 8; ++i)
        buf[i] = static_cast<std::uint8_t>(size >> (8 * i));
    if (head > 0) {
        if (auto r = read_at(h, 0, buf.data() + 8, head); !r)
            return std::unexpected(r.error());
    }
    if (tail > 0) {
        if (auto r = read_at(h, size - kSlice, buf.data() + 8 + head, tail); !r)
            return std::unexpected(r.error());
    }
    return buf;
}

std::string to_hex(const std::array<std::uint8_t, 32>& d)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string s;
    s.reserve(64);
    for (const std::uint8_t b : d) {
        s.push_back(kDigits[b >> 4]);
        s.push_back(kDigits[b & 15]);
    }
    return s;
}

}

Result<std::string> book_id(const std::filesystem::path& file)
{
    try {
        const UniqueHandle h(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!h)
            return fail_win32(GetLastError(), L"open book");
        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(h.get(), &sz))
            return fail_win32(GetLastError(), L"GetFileSizeEx");
        auto buf = sample(h.get(), static_cast<std::uint64_t>(sz.QuadPart));
        if (!buf)
            return std::unexpected(buf.error());
        std::array<std::uint8_t, 32> digest{};
        const NTSTATUS st = BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, buf->data(),
            static_cast<ULONG>(buf->size()), digest.data(), static_cast<ULONG>(digest.size()));
        if (st < 0)
            return fail(ErrorCode::Internal, L"BCryptHash failed");
        return to_hex(digest);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::NoMemory);
    } catch (...) {
        return fail(ErrorCode::Internal);
    }
}

}
