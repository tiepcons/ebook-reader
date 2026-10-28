#include "Tray.h"

#include "UiMessages.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>

namespace eb {
namespace {

constexpr wchar_t kAppName[] = L"Ebook Reader";
constexpr size_t kTipMax = 127;
constexpr UINT kTrayId = 1;
constexpr int kAppIconId = 1;

struct MenuTraits {
    using Raw = HMENU;
    static bool valid(HMENU m) noexcept { return m != nullptr; }
    static void close(HMENU m) noexcept { DestroyMenu(m); }
};
using UniqueMenu = Unique<MenuTraits>;

UniqueIcon load_icon(UINT dpi) noexcept
{
    const int cx = GetSystemMetricsForDpi(SM_CXSMICON, dpi), cy = GetSystemMetricsForDpi(SM_CYSMICON, dpi);
    HICON icon = nullptr;
    if (FAILED(LoadIconWithScaleDown(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kAppIconId), cx, cy, &icon)))
        LoadIconWithScaleDown(nullptr, IDI_APPLICATION, cx, cy, &icon);
    return UniqueIcon(icon);
}

void copy_text(wchar_t* dst, size_t cap, std::wstring_view src) noexcept
{
    const size_t n = std::min(src.size(), cap - 1);
    std::ranges::copy(src.substr(0, n), dst);
    dst[n] = L'\0';
}

}

TrayItems tray_items(TtsState s) noexcept
{
    const bool active = s == TtsState::Playing || s == TtsState::Paused;
    return {s != TtsState::Unavailable, active, active, s == TtsState::Playing ? L"Tạm dừng" : L"Đọc to"};
}

std::wstring tray_tooltip(std::wstring_view book_title)
{
    std::wstring tip = kAppName;
    if (book_title.empty())
        return tip;
    tip += L" \u2014 ";
    tip.append(book_title.substr(0, kTipMax - tip.size()));
    return tip;
}

TrayAction tray_action(LPARAM lp) noexcept
{
    switch (LOWORD(lp)) {
    case NIN_SELECT:
    case NIN_KEYSELECT:
    case WM_LBUTTONDBLCLK:
        return TrayAction::Restore;
    case WM_CONTEXTMENU:
        return TrayAction::Menu;
    default:
        return TrayAction::None;
    }
}

UINT Tray::taskbar_created_message() noexcept
{
    static const UINT msg = RegisterWindowMessageW(L"TaskbarCreated");
    return msg;
}

bool Tray::notify(DWORD cmd, UINT flags, const wchar_t* info) noexcept
{
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = owner_;
    nid.uID = kTrayId;
    nid.uFlags = flags;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = icon_.get();
    nid.uVersion = NOTIFYICON_VERSION_4;
    copy_text(nid.szTip, std::size(nid.szTip), tip_);
    if (info != nullptr) {
        copy_text(nid.szInfoTitle, std::size(nid.szInfoTitle), kAppName);
        copy_text(nid.szInfo, std::size(nid.szInfo), info);
        nid.dwInfoFlags = NIIF_INFO;
    }
    return Shell_NotifyIconW(cmd, &nid) != FALSE;
}

bool Tray::add(HWND owner, UINT dpi) noexcept
{
    owner_ = owner;
    dpi_ = dpi;
    icon_ = load_icon(dpi);
    constexpr UINT kFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    present_ = notify(NIM_ADD, kFlags) && notify(NIM_SETVERSION, 0);
    if (!present_)
        notify(NIM_DELETE, 0);
    return present_;
}

void Tray::remove() noexcept
{
    if (present_)
        notify(NIM_DELETE, 0);
    present_ = false;
}

void Tray::set_dpi(UINT dpi) noexcept
{
    if (dpi == dpi_)
        return;
    dpi_ = dpi;
    if (!present_)
        return;
    icon_ = load_icon(dpi);
    notify(NIM_MODIFY, NIF_ICON);
}

bool Tray::readd() noexcept
{
    if (!present_)
        return false;
    present_ = false;
    return add(owner_, dpi_);
}

bool Tray::update() noexcept
{
    return present_ && notify(NIM_MODIFY, NIF_TIP | NIF_SHOWTIP);
}

bool Tray::set_tooltip(std::wstring_view book_title) noexcept
{
    try {
        tip_ = tray_tooltip(book_title);
    } catch (...) {
        return false;
    }
    return update();
}

bool Tray::balloon(const wchar_t* text) noexcept
{
    return present_ && notify(NIM_MODIFY, NIF_INFO, text);
}

void Tray::show_menu(WPARAM anchor) noexcept
{
    const TrayItems items = tray_items(tts_);
    const UniqueMenu menu(CreatePopupMenu());
    if (!menu)
        return;
    const auto add_item = [&](int id, const wchar_t* text, bool enabled) {
        AppendMenuW(menu.get(), MF_STRING | (enabled ? MF_ENABLED : MF_GRAYED), static_cast<UINT_PTR>(id), text);
    };
    if (!quota_.empty()) {
        add_item(kIdTrayQuota, quota_.c_str(), false);
        AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    }
    add_item(kIdTrayOpen, L"Mở", true);
    add_item(kIdTrayPlayPause, items.play_label, items.play_pause);
    add_item(kIdTrayNext, L"Câu tiếp", items.next);
    add_item(kIdTrayStop, L"Dừng đọc", items.stop);
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    add_item(kIdTrayExit, L"Thoát", true);
    SetMenuDefaultItem(menu.get(), kIdTrayOpen, FALSE);
    POINT pt{GET_X_LPARAM(anchor), GET_Y_LPARAM(anchor)};
    if (pt.x == 0 && pt.y == 0)
        GetCursorPos(&pt);
    SetForegroundWindow(owner_);
    TrackPopupMenu(menu.get(), TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, owner_, nullptr);
    PostMessageW(owner_, WM_NULL, 0, 0);
}

}
