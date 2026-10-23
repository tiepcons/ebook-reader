#include "CrashGuard.h"

#include "AtomicFile.h"
#include "Handle.h"
#include "Log.h"

#include <windows.h>

#include <dbghelp.h>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <new>
#include <vector>

namespace eb {
namespace {

constexpr size_t kKeepDumps = 5;
constexpr size_t kMaxMarker = 4096;
constexpr UINT kCrashExit = 0xE0000001;

wchar_t g_prefix[MAX_PATH]; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): read by the crash handler
std::atomic_flag g_busy;

void write_dump(EXCEPTION_POINTERS* ep) noexcept
{
    if (g_prefix[0] == L'\0' || g_busy.test_and_set())
        return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t path[MAX_PATH + 40];
    if (swprintf_s(path, L"%s%04u%02u%02u-%02u%02u%02u-%lu.dmp", g_prefix, t.wYear, t.wMonth, t.wDay, t.wHour,
            t.wMinute, t.wSecond, GetCurrentProcessId())
        < 0)
        return;
    const UniqueHandle h(CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!h)
        return;
    MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): MINIDUMP_TYPE is a flag set
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory);
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h.get(), type, ep != nullptr ? &mei : nullptr,
        nullptr, nullptr);
}

[[noreturn]] void die(EXCEPTION_POINTERS* ep) noexcept
{
    write_dump(ep);
    TerminateProcess(GetCurrentProcess(), kCrashExit);
    std::abort();
}

LONG WINAPI on_unhandled(EXCEPTION_POINTERS* ep) noexcept
{
    die(ep);
}

void on_invalid_parameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) noexcept
{
    die(nullptr);
}

void on_purecall() noexcept
{
    die(nullptr);
}

void on_terminate() noexcept
{
    die(nullptr);
}

void prune_dumps(const std::filesystem::path& dir)
{
    std::vector<std::filesystem::path> dumps;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().wstring();
        if (name.starts_with(L"eb-") && name.ends_with(L".dmp"))
            dumps.push_back(it->path());
    }
    if (dumps.size() < kKeepDumps)
        return;
    std::ranges::sort(dumps);
    for (size_t i = 0; i + kKeepDumps - 1 < dumps.size(); ++i)
        std::filesystem::remove(dumps[i], ec);
}

}

void install_crash_handler(const std::filesystem::path& crash_dir) noexcept
{
    try {
        std::filesystem::create_directories(crash_dir);
        prune_dumps(crash_dir);
        const std::wstring prefix = (crash_dir / L"eb-").wstring();
        if (prefix.size() >= MAX_PATH) {
            log::error("crash dir path too long");
            return;
        }
        prefix.copy(g_prefix, prefix.size());
        g_prefix[prefix.size()] = L'\0';
    } catch (...) {
        log::error("crash handler install failed");
        return;
    }
    SetUnhandledExceptionFilter(&on_unhandled);
    _set_invalid_parameter_handler(&on_invalid_parameter);
    _set_purecall_handler(&on_purecall);
    std::set_terminate(&on_terminate);
}

CrashLoopGuard::CrashLoopGuard(std::filesystem::path marker_file) noexcept : marker_(std::move(marker_file))
{
    try {
        std::ifstream in(marker_, std::ios::binary);
        std::string line(kMaxMarker, '\0');
        in.read(line.data(), static_cast<std::streamsize>(line.size()));
        line.resize(static_cast<size_t>(in.gcount()));
        const size_t tab = line.find('\t');
        if (tab != std::string::npos)
            suspect_ = line.substr(tab + 1);
    } catch (...) {
        suspect_.reset();
    }
}

Result<void> CrashLoopGuard::begin(std::string_view op, std::string_view id)
{
    if (op.size() + id.size() + 1 > kMaxMarker)
        return fail(ErrorCode::Limit, L"crash marker too large");
    try {
        std::string text;
        text.reserve(op.size() + id.size() + 1);
        text.append(op).append(1, '\t').append(id);
        return write_atomic(marker_, std::as_bytes(std::span<const char>(text)));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::NoMemory);
    } catch (...) {
        return fail(ErrorCode::Internal);
    }
}

void CrashLoopGuard::end() noexcept
{
    DeleteFileW(marker_.c_str());
}

}
