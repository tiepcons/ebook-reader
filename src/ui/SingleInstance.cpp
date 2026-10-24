#include "SingleInstance.h"

#include "base/Text.h"

#include <algorithm>
#include <format>

namespace eb {
namespace {

constexpr wchar_t kPropName[] = L"EbookReader.Instance";
constexpr DWORD kForwardWaitMs = 1500;
constexpr DWORD kPollMs = 50;
constexpr UINT kSendTimeoutMs = 1000;
constexpr DWORD kMaxHoldMs = 60000;
constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

HWND find_instance_window(std::uint64_t key) noexcept
{
    HWND w = nullptr;
    while ((w = FindWindowExW(nullptr, w, kMainWndClass, nullptr)) != nullptr) {
        if (reinterpret_cast<std::uint64_t>(GetPropW(w, kPropName)) == key)
            return w;
    }
    return nullptr;
}

bool send_open(HWND target, std::wstring_view path) noexcept
{
    const size_t bytes = (path.size() + 1) * sizeof(wchar_t);
    if (bytes > kMaxCopyDataBytes)
        return false;
    try {
        std::wstring buf(path);
        DWORD pid = 0;
        GetWindowThreadProcessId(target, &pid);
        AllowSetForegroundWindow(pid);
        COPYDATASTRUCT cds{kCopyDataOpen, static_cast<DWORD>(bytes), buf.data()};
        DWORD_PTR accepted = 0;
        return SendMessageTimeoutW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG, kSendTimeoutMs,
                   &accepted) != 0
            && accepted != 0;
    } catch (...) {
        return false;
    }
}


DWORD parse_ms(std::wstring_view v) noexcept
{
    DWORD ms = 0;
    for (const wchar_t c : v) {
        if (c < L'0' || c > L'9')
            return 0;
        ms = std::min<DWORD>(ms * 10 + static_cast<DWORD>(c - L'0'), kMaxHoldMs);
    }
    return ms;
}

}

LaunchArgs parse_launch_args(std::span<const std::wstring_view> args)
{
    LaunchArgs out;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::wstring_view a = args[i];
        if (a == L"--smoke-tray") {
            out.smoke_tray = true;
        } else if (a == L"--smoke-hold" && i + 1 < args.size()) {
            out.hold_ms = parse_ms(args[++i]);
        } else if (a == L"--smoke-open") {
            ++i;
        } else if (!a.starts_with(L"--") && out.path.empty()) {
            std::error_code ec;
            const auto abs = std::filesystem::absolute(std::filesystem::path(a), ec);
            if (!ec)
                out.path = abs.lexically_normal().make_preferred().wstring();
        }
    }
    return out;
}

std::uint64_t instance_key(const std::filesystem::path& exe_dir)
{
    std::error_code ec;
    auto canon = std::filesystem::weakly_canonical(exe_dir, ec);
    if (ec)
        canon = exe_dir;
    std::uint64_t h = kFnvOffset;
    for (const wchar_t c : lower_case(canon.wstring())) {
        h = (h ^ static_cast<std::uint64_t>(c)) * kFnvPrime;
    }
    return h;
}

std::wstring mutex_name(std::uint64_t key)
{
    return std::format(L"Local\\EbookReader-{:016x}", key);
}

std::optional<std::wstring> decode_open_request(ULONG_PTR kind, const void* data, DWORD bytes) noexcept
{
    if (kind != kCopyDataOpen || data == nullptr || bytes < sizeof(wchar_t) || bytes > kMaxCopyDataBytes
        || bytes % sizeof(wchar_t) != 0)
        return std::nullopt;
    const auto* w = static_cast<const wchar_t*>(data);
    const size_t n = bytes / sizeof(wchar_t);
    const wchar_t* end = std::find(w, w + n, L'\0');
    if (end == w + n)
        return std::nullopt;
    try {
        return std::wstring(w, end);
    } catch (...) {
        return std::nullopt;
    }
}

SingleInstance::SingleInstance(std::uint64_t key) noexcept : key_(key)
{
    try {
        const std::wstring name = mutex_name(key);
        HANDLE h = CreateMutexW(nullptr, FALSE, name.c_str());
        const DWORD err = GetLastError();
        mutex_.reset(h);
        first_ = h == nullptr || err != ERROR_ALREADY_EXISTS;
    } catch (...) {
        first_ = true;
    }
}

bool SingleInstance::forward(std::wstring_view path) const noexcept
{
    const ULONGLONG deadline = GetTickCount64() + kForwardWaitMs;
    for (;;) {
        if (HWND w = find_instance_window(key_); w != nullptr)
            return send_open(w, path);
        if (GetTickCount64() >= deadline)
            return false;
        Sleep(kPollMs);
    }
}

void SingleInstance::mark_window(HWND wnd, std::uint64_t key) noexcept
{
    SetPropW(wnd, kPropName, reinterpret_cast<HANDLE>(key));
}

void SingleInstance::unmark_window(HWND wnd) noexcept
{
    RemovePropW(wnd, kPropName);
}

}
