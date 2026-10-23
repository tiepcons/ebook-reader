#include "Log.h"

#include "Handle.h"

#include <cstdint>

namespace eb::log {
namespace {

constexpr std::uint64_t kMaxBytes = 1024ULL * 1024;

SRWLOCK g_lock = SRWLOCK_INIT;
UniqueHandle g_file;
std::filesystem::path g_path;
std::filesystem::path g_rotated;
std::uint64_t g_size = 0;

bool open_file() noexcept
{
    HANDLE h = CreateFileW(g_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    g_file.reset(h);
    LARGE_INTEGER sz{};
    g_size = (g_file && GetFileSizeEx(h, &sz)) ? static_cast<std::uint64_t>(sz.QuadPart) : 0;
    return g_file.valid();
}

void rotate() noexcept
{
    g_file.reset();
    MoveFileExW(g_path.c_str(), g_rotated.c_str(), MOVEFILE_REPLACE_EXISTING);
    open_file();
}

std::string format_line(std::string_view level, std::string_view message)
{
    SYSTEMTIME t{};
    GetLocalTime(&t);
    std::string line = std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} [{}] [{}] ", t.wYear, t.wMonth,
        t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId(), level);
    line += message;
    line += '\n';
    return line;
}

}

void init(const std::filesystem::path& file) noexcept
{
    AcquireSRWLockExclusive(&g_lock);
    try {
        g_file.reset();
        g_path = file;
        g_rotated = file;
        g_rotated.replace_extension(L".1.log");
        open_file();
    } catch (...) { // NOLINT(bugprone-empty-catch): logging must never throw
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void shutdown() noexcept
{
    AcquireSRWLockExclusive(&g_lock);
    g_file.reset();
    ReleaseSRWLockExclusive(&g_lock);
}

void write(std::string_view level, std::string_view message) noexcept
{
    AcquireSRWLockExclusive(&g_lock);
    try {
        if (g_file) {
            const std::string line = format_line(level, message);
            if (g_size + line.size() > kMaxBytes)
                rotate();
            DWORD n = 0;
            if (g_file && WriteFile(g_file.get(), line.data(), static_cast<DWORD>(line.size()), &n, nullptr))
                g_size += n;
        }
    } catch (...) { // NOLINT(bugprone-empty-catch): logging must never throw
    }
    ReleaseSRWLockExclusive(&g_lock);
}

}
