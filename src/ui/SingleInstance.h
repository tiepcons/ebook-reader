#pragma once
#include "base/Handle.h"

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace eb {

inline constexpr wchar_t kMainWndClass[] = L"EbookReaderMain";
inline constexpr ULONG_PTR kCopyDataOpen = 0x45420001;
inline constexpr DWORD kMaxCopyDataBytes = 32 * 1024;

struct LaunchArgs {
    bool smoke_tray = false;
    DWORD hold_ms = 0;
    std::wstring path; // absolute; empty when the command line named no file
};

// args excludes the program name. Unknown "--" switches are ignored.
LaunchArgs parse_launch_args(std::span<const std::wstring_view> args);

// Stable per executable directory, so portable copies in different folders may run side by side.
std::uint64_t instance_key(const std::filesystem::path& exe_dir);
std::wstring mutex_name(std::uint64_t key);

// Untrusted input from another process: the payload must be NUL-terminated UTF-16 within bounds.
std::optional<std::wstring> decode_open_request(ULONG_PTR kind, const void* data, DWORD bytes) noexcept;

class SingleInstance {
public:
    explicit SingleInstance(std::uint64_t key) noexcept;
    [[nodiscard]] bool is_first() const noexcept { return first_; }
    // For a second instance: asks the running one to come forward and open path (may be empty).
    [[nodiscard]] bool forward(std::wstring_view path) const noexcept;

    static void mark_window(HWND wnd, std::uint64_t key) noexcept;
    static void unmark_window(HWND wnd) noexcept;

private:
    std::uint64_t key_;
    UniqueHandle mutex_;
    bool first_ = false;
};

}
