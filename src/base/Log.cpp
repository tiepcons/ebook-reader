#include "Log.h"

#include "Handle.h"

#include <cstdint>
#include <new>

namespace eb::log {
namespace {

constexpr std::uint64_t kMaxBytes = 1024ULL * 1024;

struct State {
    SRWLOCK lock = SRWLOCK_INIT;
    UniqueHandle file;
    std::filesystem::path path;
    std::filesystem::path rotated;
    std::uint64_t size = 0;
};

State& state() noexcept
{
    alignas(State) static unsigned char storage[sizeof(State)];
    static State* const s = new (storage) State();
    return *s;
}

bool open_file(State& st, bool truncate) noexcept
{
    const DWORD access = truncate ? (FILE_WRITE_DATA | FILE_APPEND_DATA) : FILE_APPEND_DATA;
    HANDLE h = CreateFileW(st.path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
        truncate ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    st.file.reset(h);
    LARGE_INTEGER sz{};
    st.size = (st.file && GetFileSizeEx(h, &sz)) ? static_cast<std::uint64_t>(sz.QuadPart) : 0;
    return st.file.valid();
}

void rotate(State& st) noexcept
{
    st.file.reset();
    const bool moved = MoveFileExW(st.path.c_str(), st.rotated.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    open_file(st, !moved);
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
    State& st = state();
    AcquireSRWLockExclusive(&st.lock);
    try {
        st.file.reset();
        st.path = file;
        st.rotated = file;
        st.rotated.replace_extension(L".1.log");
        open_file(st, false);
    } catch (...) { // NOLINT(bugprone-empty-catch): logging must never throw
    }
    ReleaseSRWLockExclusive(&st.lock);
}

// Call after all threads that log have been joined; the state itself is intentionally leaked.
void shutdown() noexcept
{
    State& st = state();
    AcquireSRWLockExclusive(&st.lock);
    st.file.reset();
    ReleaseSRWLockExclusive(&st.lock);
}

void write(std::string_view level, std::string_view message) noexcept
{
    State& st = state();
    AcquireSRWLockExclusive(&st.lock);
    try {
        if (st.file) {
            const std::string line = format_line(level, message);
            if (st.size + line.size() > kMaxBytes)
                rotate(st);
            DWORD n = 0;
            if (st.file && WriteFile(st.file.get(), line.data(), static_cast<DWORD>(line.size()), &n, nullptr))
                st.size += n;
        }
    } catch (...) { // NOLINT(bugprone-empty-catch): logging must never throw
    }
    ReleaseSRWLockExclusive(&st.lock);
}

}
