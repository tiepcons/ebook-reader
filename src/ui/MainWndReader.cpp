#include "MainWnd.h"

#include "base/Log.h"
#include "base/Text.h"
#include "ReaderSettings.h"
#include "lib/LibraryModel.h"

#include <shellapi.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <memory>

namespace eb {
namespace {

constexpr int kBarMargin = 6, kBarH = 26;
constexpr int kBackW = 120, kResumeTextW = 240, kResumeBtnW = 80, kAaW = 44;
constexpr UINT kResumeBarMs = 6000;
constexpr UINT kReaderTickMs = 500;
constexpr UINT kSmokeWatchdogMs = 30000;
constexpr int kSmokeKeys = 3;
constexpr int kStatusBad = 2;

struct ArgvFree {
    void operator()(wchar_t** p) const noexcept { LocalFree(static_cast<void*>(p)); }
};

void show_all(std::initializer_list<HWND> list, int cmd) noexcept
{
    for (HWND h : list)
        ShowWindow(h, cmd);
}

std::string smoke_open_name()
{
    int argc = 0;
    const std::unique_ptr<wchar_t*, ArgvFree> argv(CommandLineToArgvW(GetCommandLineW(), &argc));
    for (int i = 1; argv && i + 1 < argc; ++i) {
        if (std::wstring_view(argv.get()[i]) == L"--smoke-open")
            return wide_to_utf8(std::filesystem::path(argv.get()[i + 1]).filename().wstring()).value_or(std::string{});
    }
    return {};
}

bool has_arg(std::wstring_view flag)
{
    int argc = 0;
    const std::unique_ptr<wchar_t*, ArgvFree> argv(CommandLineToArgvW(GetCommandLineW(), &argc));
    for (int i = 1; argv && i < argc; ++i) {
        if (std::wstring_view(argv.get()[i]) == flag)
            return true;
    }
    return false;
}

}

bool MainWnd::create_reader_children()
{
    const auto child = [this](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
        return CreateWindowExW(0, cls, text, WS_CHILD | style, 0, 0, 0, 0, hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst_, nullptr); // NOLINT(performance-no-int-to-ptr): Win32 message ABI
    };
    back_ = child(L"BUTTON", L"← Thư viện", BS_PUSHBUTTON | WS_TABSTOP, kIdBack);
    resume_text_ = child(L"STATIC", L"", SS_CENTERIMAGE, kIdResumeText);
    resume_btn_ = child(L"BUTTON", L"Về đầu", BS_PUSHBUTTON | WS_TABSTOP, kIdResumeStart);
    aa_ = child(L"BUTTON", L"Aa", BS_PUSHBUTTON | WS_TABSTOP, kIdReaderSettings);
    return back_ != nullptr && resume_text_ != nullptr && resume_btn_ != nullptr && aa_ != nullptr && reader_.create(inst_, hwnd_);
}

void MainWnd::layout_reader()
{
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    split_rc_ = {};
    const int m = px(kBarMargin), ch = px(kBarH);
    int status_h = 0, top = 0;
    if (!fullscreen_) {
        SendMessageW(status_, WM_SIZE, 0, 0);
        RECT sr{};
        GetWindowRect(status_, &sr);
        status_h = sr.bottom - sr.top;
        top = ch + 2 * m;
    }
    const int tx = 2 * m + px(kBackW);
    MoveWindow(back_, m, m, px(kBackW), ch, TRUE);
    MoveWindow(resume_text_, tx, m, px(kResumeTextW), ch, TRUE);
    MoveWindow(resume_btn_, tx + px(kResumeTextW) + m, m, px(kResumeBtnW), ch, TRUE);
    MoveWindow(aa_, std::max(0L, rc.right - m - px(kAaW)), m, px(kAaW), ch, TRUE);
    MoveWindow(reader_.hwnd(), 0, top, rc.right, std::max(0L, rc.bottom - top - status_h), TRUE);
}

void MainWnd::show_notice(const wchar_t* text)
{
    log::info("notice: {}", wide_to_utf8(text).value_or(std::string{}));
    if (!smoke_)
        MessageBoxW(hwnd_, text, L"Ebook Reader", MB_OK | MB_ICONWARNING);
}

void MainWnd::open_book(const std::string& id)
{
    if (in_reader_)
        return;
    std::string fmt;
    int status = -1;
    if (auto st = db_.prepare("SELECT fmt,status FROM books WHERE id=?1"); st && st->bind_text(1, id)) {
        if (const auto step = st->step(); step && *step == Step::Row) {
            fmt = std::string(st->column_text(0));
            status = static_cast<int>(st->column_int(1));
        }
    }
    const auto path = book_path(db_, id);
    if (status < 0 || !path) {
        show_notice(L"Không tìm thấy sách trong thư viện.");
        return;
    }
    if (status == 1 || status == 3) {
        show_notice(status == 1 ? L"Không tìm thấy tệp sách. Hãy quét lại thư viện." : L"Sách đã bị ẩn khỏi thư viện.");
        return;
    }
    in_reader_ = true;
    reading_id_ = id;
    show_all({search_, sort_, rescan_, open_, tree_, list_}, SW_HIDE);
    show_all({back_, aa_, reader_.hwnd()}, SW_SHOW);
    layout();
    if (!reader_.open(OpenJob{*path, paths_.data / L"pending", id, fmt})) {
        close_book();
        show_notice(L"Không mở được sách: không đủ bộ nhớ.");
        return;
    }
    apply_chrome();
    SetTimer(hwnd_, kReaderTickTimer, kReaderTickMs, nullptr);
    SetFocus(reader_.hwnd());
    SendMessageW(status_, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(reader_.status_text().c_str()));
}

void MainWnd::close_book()
{
    if (!in_reader_)
        return;
    if (fullscreen_)
        toggle_fullscreen();
    KillTimer(hwnd_, kReaderTickTimer);
    hide_resume();
    reader_.close();
    in_reader_ = false;
    apply_chrome();
    show_all({back_, aa_, reader_.hwnd()}, SW_HIDE);
    show_all({search_, sort_, rescan_, open_, tree_, list_}, SW_SHOW);
    layout();
    refresh_list();
    SetFocus(list_);
}

void MainWnd::on_reader_event(ReaderEvent e, int page)
{
    if (!in_reader_)
        return;
    const bool smoke_run = smoke_ && !smoke_open_.empty();
    switch (e) {
    case ReaderEvent::PageShown:
    case ReaderEvent::PageSettled:
        SendMessageW(status_, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(reader_.status_text().c_str()));
        if (resume_page_ >= 0 && page != resume_page_)
            hide_resume();
        if (e == ReaderEvent::PageSettled) {
            if (const int p = reader_.take_resume_percent(); p >= 0)
                show_resume(p, page);
            smoke_step(page);
        }
        break;
    case ReaderEvent::OpenFailed: {
        log::error("reader: open failed for {}", reading_id_);
        if (auto st = db_.prepare("UPDATE books SET status=?2 WHERE id=?1"); st && st->bind_text(1, reading_id_) && st->bind_int(2, kStatusBad))
        {
            if (!st->step())
                log::error("reader: cannot mark book as bad");
        }
        const std::wstring msg = reader_.error_text();
        close_book();
        show_notice(msg.c_str());
        if (smoke_run)
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        break;
    }
    case ReaderEvent::CloseRequested:
        if (fullscreen_) {
            toggle_fullscreen();
        } else {
            close_book();
            if (smoke_run)
                PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        }
        break;
    case ReaderEvent::Fullscreen:
        toggle_fullscreen();
        break;
    case ReaderEvent::Settings:
        open_reader_settings();
        break;
    }
}

bool MainWnd::on_reader_msg(UINT msg, WPARAM wp, LPARAM lp, LRESULT& res)
{
    res = 0;
    switch (msg) {
    case WM_APP_READER_EVENT:
        on_reader_event(static_cast<ReaderEvent>(wp), static_cast<int>(lp));
        return true;
    case kMsgSmokeOpen:
        smoke_open_book();
        return true;
    case WM_COMMAND:
        if (LOWORD(wp) == kIdBack && HIWORD(wp) == BN_CLICKED) {
            close_book();
        } else if (LOWORD(wp) == kIdReaderSettings && HIWORD(wp) == BN_CLICKED) {
            open_reader_settings();
        } else if (LOWORD(wp) == kIdResumeStart && HIWORD(wp) == BN_CLICKED) {
            reader_.go_to_start();
            hide_resume();
            SetFocus(reader_.hwnd());
        } else {
            return false;
        }
        return true;
    case WM_TIMER:
        if (wp == kReaderTickTimer)
            reader_.tick();
        else if (wp == kResumeBarTimer)
            hide_resume();
        else if (wp == kSmokeTimer)
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        else
            return false;
        return true;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE)
            reader_.flush();
        return false;
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED)
            reader_.flush();
        return false;
    case WM_SETFOCUS:
        if (!in_reader_)
            return false;
        SetFocus(reader_.hwnd());
        return true;
    default:
        return false;
    }
}

void MainWnd::toggle_fullscreen()
{
    if (!in_reader_)
        return;
    if (!fullscreen_) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        saved_wp_.length = sizeof(saved_wp_);
        if (!GetWindowPlacement(hwnd_, &saved_wp_) || !GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi))
            return;
        saved_style_ = GetWindowLongPtrW(hwnd_, GWL_STYLE);
        saved_menu_ = GetMenu(hwnd_);
        fullscreen_ = true;
        SetMenu(hwnd_, nullptr);
        SetWindowLongPtrW(hwnd_, GWL_STYLE, (saved_style_ & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW)) | WS_POPUP);
        SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        fullscreen_ = false;
        SetWindowLongPtrW(hwnd_, GWL_STYLE, saved_style_);
        SetMenu(hwnd_, saved_menu_);
        SetWindowPlacement(hwnd_, &saved_wp_);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    const int chrome = fullscreen_ ? SW_HIDE : SW_SHOW;
    show_all({status_, back_, aa_}, chrome);
    show_all({resume_text_, resume_btn_}, resume_page_ >= 0 ? chrome : SW_HIDE);
    layout();
}

void MainWnd::show_resume(int percent, int page)
{
    const std::wstring t = std::format(L"Tiếp tục từ {} %", percent);
    SetWindowTextW(resume_text_, t.c_str());
    resume_page_ = page;
    if (!fullscreen_)
        show_all({resume_text_, resume_btn_}, SW_SHOW);
    SetTimer(hwnd_, kResumeBarTimer, kResumeBarMs, nullptr);
}

void MainWnd::hide_resume()
{
    KillTimer(hwnd_, kResumeBarTimer);
    resume_page_ = -1;
    show_all({resume_text_, resume_btn_}, SW_HIDE);
}

void MainWnd::init_smoke_open()
{
    if (!smoke_)
        return;
    smoke_open_ = smoke_open_name();
    if (smoke_open_.empty())
        return;
    smoke_theme_ = has_arg(L"--smoke-theme");
    SetTimer(hwnd_, kSmokeTimer, kSmokeWatchdogMs, nullptr);
    PostMessageW(hwnd_, kMsgSmokeOpen, 0, 0);
}

void MainWnd::smoke_open_book()
{
    if (auto st = db_.prepare("SELECT id FROM books WHERE status=0 AND path LIKE ?1"); st && st->bind_text(1, "%\\" + smoke_open_)) {
        if (const auto step = st->step(); step && *step == Step::Row) {
            open_book(std::string(st->column_text(0)));
            if (in_reader_)
                return;
        }
    }
    log::error("smoke: cannot open {}", smoke_open_);
    PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

void MainWnd::smoke_step(int page)
{
    if (smoke_open_.empty())
        return;
    if (smoke_theme_) {
        smoke_theme_ = false;
        ReaderPrefs p = reader_.prefs();
        p.theme.id = ThemeId::Black;
        reader_.set_prefs(p, false);
        apply_chrome();
        return;
    }
    if (!smoke_keys_sent_ && page == 0) {
        smoke_keys_sent_ = true;
        for (int i = 0; i < kSmokeKeys; ++i)
            PostMessageW(reader_.hwnd(), WM_KEYDOWN, VK_RIGHT, 0);
    } else if (smoke_keys_sent_ && page >= kSmokeKeys) {
        PostMessageW(reader_.hwnd(), WM_KEYDOWN, VK_ESCAPE, 0);
    }
}

void MainWnd::apply_chrome()
{
    apply_reader_chrome(hwnd_, status_, in_reader_ ? std::optional<uint32_t>(reader_.colors().bg) : std::nullopt);
}

void MainWnd::open_reader_settings()
{
    if (!in_reader_ || in_settings_)
        return;
    in_settings_ = true;
    const ReaderPrefs initial = reader_.prefs();
    const PrefsApply live = [this](const ReaderPrefs& p) {
        reader_.set_prefs(p, false);
        apply_chrome();
    };
    const auto accepted = show_reader_settings(inst_, hwnd_, initial, live);
    reader_.set_prefs(accepted ? *accepted : initial, accepted.has_value());
    apply_chrome();
    in_settings_ = false;
    SetFocus(reader_.hwnd());
}

}
