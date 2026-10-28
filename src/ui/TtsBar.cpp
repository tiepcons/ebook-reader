#include "TtsBar.h"

#include "UiMessages.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace eb {
namespace {

constexpr int kStepW = 96, kPlayW = 104, kStopW = 70, kSpeedW = 78, kSettingsW = 86;
constexpr int kSpeedDropH = 200;

HWND make(HINSTANCE inst, HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id)
{
    return CreateWindowExW(0, cls, text, WS_CHILD | style, 0, 0, 0, 0, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr); // NOLINT(performance-no-int-to-ptr): Win32 message ABI
}

}

int nearest_speed_index(double rate) noexcept
{
    int best = 0;
    for (int i = 1; i < static_cast<int>(kTtsSpeeds.size()); ++i) {
        if (std::fabs(kTtsSpeeds[static_cast<std::size_t>(i)] - rate) < std::fabs(kTtsSpeeds[static_cast<std::size_t>(best)] - rate))
            best = i;
    }
    return best;
}

bool TtsBar::create(HINSTANCE inst, HWND parent) noexcept
{
    constexpr DWORD kBtn = BS_PUSHBUTTON | WS_TABSTOP;
    prev_ = make(inst, parent, L"BUTTON", L"◀ Câu trước", kBtn, kIdTtsPrev);
    play_ = make(inst, parent, L"BUTTON", L"▶ Đọc", kBtn, kIdTtsPlay);
    stop_ = make(inst, parent, L"BUTTON", L"■ Dừng", kBtn, kIdTtsStop);
    next_ = make(inst, parent, L"BUTTON", L"Câu sau ▶", kBtn, kIdTtsNext);
    speed_ = make(inst, parent, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kIdTtsSpeed);
    settings_ = make(inst, parent, L"BUTTON", L"Giọng đọc…", kBtn, kIdTtsSettings);
    info_ = make(inst, parent, L"STATIC", L"", SS_CENTERIMAGE | SS_ENDELLIPSIS, kIdTtsInfo);
    for (HWND h : {prev_, play_, stop_, next_, speed_, settings_, info_}) {
        if (h == nullptr)
            return false;
    }
    for (const wchar_t* label : kTtsSpeedLabels)
        SendMessageW(speed_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
    set_speed(1.0);
    return true;
}

void TtsBar::layout(int x, int y, int w, int h, int gap, UINT dpi) noexcept
{
    const auto px = [dpi](int dip) { return MulDiv(dip, static_cast<int>(dpi), 96); };
    int cx = x;
    const auto put = [&](HWND c, int cw, int ch) {
        MoveWindow(c, cx, y, cw, ch, TRUE);
        cx += cw + gap;
    };
    put(prev_, px(kStepW), h);
    put(play_, px(kPlayW), h);
    put(stop_, px(kStopW), h);
    put(next_, px(kStepW), h);
    put(speed_, px(kSpeedW), h + px(kSpeedDropH));
    put(settings_, px(kSettingsW), h);
    MoveWindow(info_, cx, y, std::max(0, x + w - cx), h, TRUE);
}

void TtsBar::show(int cmd) noexcept
{
    for (HWND h : {prev_, play_, stop_, next_, speed_, settings_, info_})
        ShowWindow(h, cmd);
}

void TtsBar::set_font(HFONT font) noexcept
{
    for (HWND h : {prev_, play_, stop_, next_, speed_, settings_, info_})
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void TtsBar::set_state(TtsState s) noexcept
{
    SetWindowTextW(play_, s == TtsState::Playing ? L"⏸ Tạm dừng" : L"▶ Đọc");
    const BOOL active = s == TtsState::Playing || s == TtsState::Paused ? TRUE : FALSE;
    for (HWND h : {stop_, prev_, next_})
        EnableWindow(h, active);
}

void TtsBar::set_info(const std::wstring& text) noexcept
{
    SetWindowTextW(info_, text.c_str());
}

void TtsBar::set_speed(double rate) noexcept
{
    SendMessageW(speed_, CB_SETCURSEL, static_cast<WPARAM>(nearest_speed_index(rate)), 0);
}

double TtsBar::speed() const noexcept
{
    const LRESULT i = SendMessageW(speed_, CB_GETCURSEL, 0, 0);
    return i >= 0 && i < static_cast<LRESULT>(kTtsSpeeds.size()) ? kTtsSpeeds[static_cast<std::size_t>(i)] : 1.0;
}

}
