#include "CrashGuard.h"

#include "AtomicFile.h"
#include "FileIo.h"
#include "Handle.h"
#include "Log.h"

#include <windows.h>

#include <dbghelp.h>
#include <atomic>
#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <new>
#include <vector>

namespace eb {
namespace {

constexpr size_t kKeepDumps = 5;
constexpr size_t kMaxMarker = 4096;
constexpr UINT kCrashExit = 0xE0000001;
constexpr DWORD kFatalCode = 0xE0EB0001;
constexpr DWORD kWatchdogStack = 256 * 1024;
constexpr DWORD kDumpWaitMs = 30000;

// State read by the crash path lives in static storage so the handler never allocates.
wchar_t g_prefix[MAX_PATH]; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
EXCEPTION_POINTERS* g_ep = nullptr; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
DWORD g_tid = 0; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
HANDLE g_request = nullptr; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
HANDLE g_done = nullptr; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic_flag g_busy;
std::atomic_flag g_installed;

void write_dump() noexcept
{
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
    MINIDUMP_EXCEPTION_INFORMATION mei{g_tid, g_ep, FALSE};
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h.get(), MiniDumpWithThreadInfo, &mei, nullptr,
        nullptr);
}

DWORD WINAPI watchdog(LPVOID) noexcept
{
    WaitForSingleObject(g_request, INFINITE);
    write_dump();
    SetEvent(g_done);
    return 0;
}

LONG WINAPI on_unhandled(EXCEPTION_POINTERS* ep) noexcept
{
    if (g_busy.test_and_set())
        Sleep(INFINITE);
    g_ep = ep;
    g_tid = GetCurrentThreadId();
    SetEvent(g_request);
    WaitForSingleObject(g_done, kDumpWaitMs);
    TerminateProcess(GetCurrentProcess(), kCrashExit);
    return EXCEPTION_EXECUTE_HANDLER;
}

[[noreturn]] void raise_fatal() noexcept
{
    RaiseException(kFatalCode, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    TerminateProcess(GetCurrentProcess(), kCrashExit);
    std::abort();
}

void on_invalid_parameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) noexcept
{
    raise_fatal();
}

void on_purecall() noexcept
{
    raise_fatal();
}

void on_terminate() noexcept
{
    raise_fatal();
}

void on_abort(int) noexcept
{
    raise_fatal();
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

bool prepare_dump_dir(const std::filesystem::path& crash_dir)
{
    std::filesystem::create_directories(crash_dir);
    prune_dumps(crash_dir);
    const std::wstring prefix = (crash_dir / L"eb-").wstring();
    if (prefix.size() >= MAX_PATH)
        return false;
    prefix.copy(g_prefix, prefix.size());
    g_prefix[prefix.size()] = L'\0';
    return true;
}

bool start_watchdog() noexcept
{
    g_request = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_request == nullptr || g_done == nullptr)
        return false;
    const UniqueHandle thread(CreateThread(nullptr, kWatchdogStack, &watchdog, nullptr, 0, nullptr));
    return thread.valid();
}

}

void install_crash_handler(const std::filesystem::path& crash_dir) noexcept
{
    if (g_installed.test_and_set())
        return;
    try {
        if (!prepare_dump_dir(crash_dir)) {
            log::error("crash dir path too long");
            return;
        }
    } catch (...) {
        log::error("crash handler install failed");
        return;
    }
    if (!start_watchdog()) {
        log::error("crash watchdog start failed");
        return;
    }
    SetUnhandledExceptionFilter(&on_unhandled);
    _set_invalid_parameter_handler(&on_invalid_parameter);
    _set_purecall_handler(&on_purecall);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    std::signal(SIGABRT, &on_abort);
    std::set_terminate(&on_terminate);
}

CrashLoopGuard::CrashLoopGuard(std::filesystem::path marker_file) noexcept : marker_(std::move(marker_file))
{
    try {
        const std::string line = read_head(marker_, kMaxMarker);
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
