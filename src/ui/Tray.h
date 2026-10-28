#pragma once
#include "base/Handle.h"

#include <windows.h>

#include <string>
#include <string_view>

namespace eb {

enum class TtsState : unsigned char { Unavailable, Idle, Playing, Paused };
enum class TtsCmd : unsigned char { PlayPause, Next, Stop }; // WM_APP_TTS_CMD wParam

inline constexpr int kIdTrayOpen = 500;
inline constexpr int kIdTrayPlayPause = 501;
inline constexpr int kIdTrayNext = 502;
inline constexpr int kIdTrayStop = 503;
inline constexpr int kIdTrayExit = 504;
inline constexpr int kIdTrayQuota = 505;

struct TrayItems {
    bool play_pause, next, stop;
    const wchar_t* play_label;
};

TrayItems tray_items(TtsState s) noexcept;
// "Ebook Reader" or "Ebook Reader — <title>", cut to fit the 127-character tooltip limit.
std::wstring tray_tooltip(std::wstring_view book_title);

enum class TrayAction : unsigned char { None, Restore, Menu };
// lp is the v4 callback lParam: LOWORD carries the notification.
TrayAction tray_action(LPARAM lp) noexcept;

struct IconTraits {
    using Raw = HICON;
    static bool valid(HICON i) noexcept { return i != nullptr; }
    static void close(HICON i) noexcept { DestroyIcon(i); }
};
using UniqueIcon = Unique<IconTraits>;

// One notification-area icon (uID 1). Callbacks arrive on the owner window as WM_APP_TRAY.
class Tray {
public:
    Tray() = default;
    Tray(const Tray&) = delete;
    Tray& operator=(const Tray&) = delete;
    ~Tray() { remove(); }

    bool add(HWND owner, UINT dpi) noexcept;
    void remove() noexcept;
    // Reloads the icon at the new small-icon size.
    void set_dpi(UINT dpi) noexcept;
    // Explorer restarted: the icon is gone and must be added again.
    bool readd() noexcept;
    [[nodiscard]] bool present() const noexcept { return present_; }

    // Stores the tooltip; pushes it to the shell when the icon is present. Returns the shell's answer.
    bool set_tooltip(std::wstring_view book_title) noexcept;
    bool update() noexcept;
    bool balloon(const wchar_t* text) noexcept;
    void set_tts_state(TtsState s) noexcept { tts_ = s; }
    // Shown as a disabled first line of the menu; empty hides it.
    void set_quota_text(std::wstring text) noexcept { quota_ = std::move(text); }
    void show_menu(WPARAM anchor) noexcept;

    static UINT taskbar_created_message() noexcept;

private:
    bool notify(DWORD cmd, UINT flags, const wchar_t* info = nullptr) noexcept;

    HWND owner_ = nullptr;
    UINT dpi_ = 96;
    bool present_ = false;
    TtsState tts_ = TtsState::Unavailable;
    std::wstring tip_ = L"Ebook Reader";
    std::wstring quota_;
    UniqueIcon icon_;
};

}
