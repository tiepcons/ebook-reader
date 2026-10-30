#include "MainWnd.h"

#include "base/Log.h"
#include "base/PathSafety.h"
#include "base/Text.h"
#include "GotoPercent.h"
#include "ReaderSettings.h"
#include "resource.h"
#include "lib/LibraryModel.h"

#include <shellapi.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <memory>
#include <span>
#include <utility>

namespace eb {
namespace {

constexpr int kTocW = 260;
constexpr int kMaxIndent = 8;
constexpr std::size_t kMaxChapterItems = 2000;
constexpr int kBackW = 120, kResumeTextW = 150, kResumeBtnW = 80;
constexpr UINT kResumeBarMs = 6000;
constexpr UINT kReaderTickMs = 500;
constexpr int kStatusBad = 2;

// Book ids are hex digests; anything else gets no cache rather than a path built from untrusted text.
std::filesystem::path cache_file(const std::filesystem::path& dir, const std::string& id)
{
    const bool plain = !id.empty() && std::ranges::all_of(id, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    return plain && is_plain_dir_chain(dir) ? dir / (std::wstring(id.begin(), id.end()) + L".lc") : std::filesystem::path{};
}

void show_all(std::initializer_list<HWND> list, int cmd) noexcept
{
    for (HWND h : list)
        ShowWindow(h, cmd);
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
    chapter_combo_ = child(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kIdChapterCombo);
    return back_ != nullptr && resume_text_ != nullptr && resume_btn_ != nullptr && aa_ != nullptr && chapter_combo_ != nullptr
        && create_bar_buttons() && reader_.create(inst_, hwnd_)
        && toc_pane_.create(inst_, hwnd_, kIdTocPane) && tts_bar_.create(inst_, hwnd_);
}

void MainWnd::layout_reader()
{
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    split_rc_ = {};
    const int m = px(kBarMarginDip), ch = px(kBarHeightDip);
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
    layout_top_bar(rc, tx + px(kResumeTextW) + m + px(kResumeBtnW) + m);
    const int pane_w = toc_shown() ? std::min(px(kTocW), static_cast<int>(rc.right) / 2) : 0;
    const int bar_h = fullscreen_ ? 0 : ch + 2 * m;
    const int body_h = static_cast<int>(std::max(0L, rc.bottom - top - status_h - bar_h));
    layout_tts_bar(rc, status_h, m, ch);
    ShowWindow(toc_pane_.hwnd(), pane_w > 0 ? SW_SHOW : SW_HIDE);
    MoveWindow(toc_pane_.hwnd(), 0, top, pane_w, body_h, TRUE);
    MoveWindow(reader_.hwnd(), pane_w, top, rc.right - pane_w, body_h, TRUE);
}

void MainWnd::layout_tts_bar(const RECT& rc, int status_h, int margin, int bar_h)
{
    tts_bar_.layout(margin, rc.bottom - status_h - bar_h - margin, static_cast<int>(rc.right) - 2 * margin, bar_h, margin, dpi_);
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
    tts_bar_.show(SW_SHOW);
    load_tts_prefs_once();
    set_tts_state(TtsState::Idle);
    layout();
    if (!reader_.open(OpenJob{*path, paths_.data / L"pending", id, fmt, cache_file(paths_.cache, id)})) {
        close_book();
        show_notice(L"Không mở được sách: không đủ bộ nhớ.");
        return;
    }
    apply_chrome();
    if (toc_visible_)
        refresh_toc_pane();
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
    tts_stop();
    reader_.close();
    SendMessageW(chapter_combo_, CB_RESETCONTENT, 0, 0);
    chapter_count_ = 0;
    reset_bar_marked();
    nav_idx_ = -1;
    nav_pending_ = false;
    set_tts_state(TtsState::Unavailable);
    in_reader_ = false;
    apply_chrome();
    show_all({back_, aa_, chapter_combo_, chapter_prev_, chapter_next_, toc_btn_, mark_btn_, reader_.hwnd(), toc_pane_.hwnd()}, SW_HIDE);
    tts_bar_.show(SW_HIDE);
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
        on_tts_reader_event(e, page);
        [[fallthrough]];
    case ReaderEvent::PageSettled:
        on_page_event(e, page);
        break;
    case ReaderEvent::OpenFailed:
        on_open_failed();
        break;
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
    case ReaderEvent::Toc:
    case ReaderEvent::Bookmark:
    case ReaderEvent::GoTo:
        on_shortcut_event(e);
        break;
    case ReaderEvent::Renumbered:
        SendMessageW(status_, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(reader_.status_text().c_str()));
        [[fallthrough]];
    case ReaderEvent::Text:
    case ReaderEvent::Relayout:
    case ReaderEvent::Relaid:
        on_tts_reader_event(e, page);
        break;
    }
}

void MainWnd::on_page_event(ReaderEvent e, int page)
{
    SendMessageW(status_, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(reader_.status_text().c_str()));
    if (resume_page_ >= 0 && page != resume_page_)
        hide_resume();
    if (toc_visible_ && reader_.content_version() != pane_version_)
        refresh_toc_pane();
    if (e != ReaderEvent::PageSettled)
        return;
    if (std::exchange(nav_pending_, false))
        nav_page_ = page;
    sync_chapter_combo();
    update_bar_buttons();
    if (const int p = reader_.take_resume_percent(); p >= 0)
        show_resume(p, page);
    smoke_step(page);
}

void MainWnd::on_tts_reader_event(ReaderEvent e, int page)
{
    if (e == ReaderEvent::Text)
        on_tts_text(page);
    else if (!tts_)
        return;
    else if (e == ReaderEvent::Relayout)
        tts_->relayout();
    else if (e == ReaderEvent::Relaid)
        tts_->relaid();
    else if (e == ReaderEvent::Renumbered)
        tts_->shift_pages(page);
    else
        tts_->page_shown();
}

void MainWnd::on_open_failed()
{
    log::error("reader: open failed for {}", reading_id_);
    if (auto st = db_.prepare("UPDATE books SET status=?2 WHERE id=?1"); st && st->bind_text(1, reading_id_) && st->bind_int(2, kStatusBad)) {
        if (!st->step())
            log::error("reader: cannot mark book as bad");
    }
    const std::wstring msg = reader_.error_text();
    close_book();
    show_notice(msg.c_str());
    if (smoke_ && !smoke_open_.empty())
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
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
        return on_reader_command(wp);
    case WM_TIMER:
        if (wp == kReaderTickTimer)
            reader_.tick();
        else if (wp == kResumeBarTimer)
            hide_resume();
        else if (wp == kSoakTimer)
            soak_tick();
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
    case WM_ENTERSIZEMOVE:
    case WM_EXITSIZEMOVE:
        reader_.set_live_resize(msg == WM_ENTERSIZEMOVE);
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

bool MainWnd::on_reader_command(WPARAM wp)
{
    const int id = LOWORD(wp);
    if (on_tts_command(wp))
        return true;
    if (id == kIdChapterCombo) {
        on_chapter_combo(HIWORD(wp));
        return true;
    }
    if (HIWORD(wp) != BN_CLICKED) // menu commands carry 0 here as well
        return false;
    if (on_bar_command(id))
        return true;
    if (id == kIdBack) {
        close_book();
    } else if (id == kIdReaderSettings) {
        open_reader_settings();
    } else if (id == kIdResumeStart) {
        reader_.go_to_start();
        hide_resume();
        SetFocus(reader_.hwnd());
    } else if (id == IDM_READER_TOC) {
        on_shortcut_event(ReaderEvent::Toc);
    } else if (id == IDM_READER_BOOKMARK) {
        on_shortcut_event(ReaderEvent::Bookmark);
    } else if (id == IDM_READER_GOTO) {
        on_shortcut_event(ReaderEvent::GoTo);
    } else {
        return false;
    }
    return true;
}

void MainWnd::on_shortcut_event(ReaderEvent e)
{
    if (!in_reader_)
        return;
    if (e == ReaderEvent::Toc)
        toggle_toc_pane();
    else if (e == ReaderEvent::Bookmark)
        toggle_bookmark();
    else
        ask_goto();
}

void MainWnd::toggle_toc_pane()
{
    toc_visible_ = !toc_visible_;
    if (toc_visible_)
        refresh_toc_pane();
    layout();
    SetFocus(toc_visible_ ? toc_pane_.hwnd() : reader_.hwnd());
}

void MainWnd::refresh_toc_pane()
{
    toc_pane_.set_content(reader_.toc(), reader_.bookmarks());
    if (smoke_)
        log::info("reader: outline pane {} entries", reader_.toc().size());
    pane_version_ = reader_.content_version();
}

void MainWnd::toggle_bookmark()
{
    const auto added = reader_.toggle_bookmark();
    const wchar_t* text = !added ? L"Không lưu được dấu trang" : (*added ? L"Đã thêm dấu trang" : L"Đã bỏ dấu trang");
    SendMessageW(status_, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(text));
    update_bar_buttons();
    if (toc_visible_)
        refresh_toc_pane();
}

void MainWnd::ask_goto()
{
    if (const auto pct = ask_percent(inst_, hwnd_, reader_.percent()))
        reader_.go_to_percent(*pct);
    SetFocus(reader_.hwnd());
}

void MainWnd::jump_to_mark(uint64_t mark)
{
    reader_.go_to_mark(mark);
    SetFocus(reader_.hwnd());
}

void MainWnd::jump_to_pick()
{
    if (const auto pick = toc_pane_.selected())
        jump_to_mark(pick->mark);
}

void MainWnd::fill_chapter_combo(std::span<const TocItem> toc)
{
    SendMessageW(chapter_combo_, WM_SETREDRAW, FALSE, 0);
    SendMessageW(chapter_combo_, CB_RESETCONTENT, 0, 0);
    SendMessageW(chapter_combo_, CB_INITSTORAGE, toc.size(), static_cast<LPARAM>(toc.size() * 64));
    for (const TocItem& it : toc) {
        const std::wstring title = utf8_to_wide(it.title).value_or(std::wstring{});
        const std::wstring label = std::wstring(2 * static_cast<std::size_t>(std::clamp(it.level, 0, kMaxIndent)), L' ') + title;
        SendMessageW(chapter_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    SendMessageW(chapter_combo_, WM_SETREDRAW, TRUE, 0);
    chapter_count_ = toc.size();
    layout();
}

void MainWnd::sync_chapter_combo()
{
    if (SendMessageW(chapter_combo_, CB_GETDROPPEDSTATE, 0, 0) != 0)
        return;
    const std::vector<TocItem>& all = reader_.toc();
    const std::span<const TocItem> toc = std::span(all).first(std::min(all.size(), kMaxChapterItems));
    if (toc.size() != chapter_count_)
        fill_chapter_combo(toc);
    const uint64_t mark = reader_.mark();
    const int remembered = remembered_toc_index();
    const int idx = remembered >= 0 && static_cast<std::size_t>(remembered) < toc.size() ? remembered : mark == kNoAnchor ? -1 : toc_index_at(toc, mark);
    if (idx >= 0 && idx != static_cast<int>(SendMessageW(chapter_combo_, CB_GETCURSEL, 0, 0)))
        SendMessageW(chapter_combo_, CB_SETCURSEL, static_cast<WPARAM>(idx), 0);
}

void MainWnd::on_chapter_combo(int code)
{
    const bool dropped = SendMessageW(chapter_combo_, CB_GETDROPPEDSTATE, 0, 0) != 0;
    if (code != CBN_SELENDOK && !(code == CBN_SELCHANGE && !dropped))
        return;
    const LRESULT sel = SendMessageW(chapter_combo_, CB_GETCURSEL, 0, 0);
    const std::vector<TocItem>& toc = reader_.toc();
    if (sel >= 0 && static_cast<std::size_t>(sel) < toc.size())
        jump_to_toc(static_cast<int>(sel));
}

void MainWnd::on_toc_notify(NMHDR& hdr)
{
    if (hdr.code == NM_DBLCLK) {
        jump_to_pick();
        return;
    }
    if (hdr.code != TVN_KEYDOWN)
        return;
    const unsigned vk = reinterpret_cast<const NMTVKEYDOWN&>(hdr).wVKey;
    if (GetKeyState(VK_CONTROL) < 0) {
        const NavAction a = nav_for_key(vk, false, true);
        if (a == NavAction::Toc)
            on_shortcut_event(ReaderEvent::Toc);
        else if (a == NavAction::Bookmark)
            on_shortcut_event(ReaderEvent::Bookmark);
        else if (a == NavAction::GoTo)
            on_shortcut_event(ReaderEvent::GoTo);
    } else if (vk == VK_RETURN) {
        jump_to_pick();
    } else if (vk == VK_ESCAPE) {
        SetFocus(reader_.hwnd());
    } else if (const auto pick = toc_pane_.selected(); vk == VK_DELETE && pick && pick->bookmark_id != 0) {
        reader_.remove_bookmark(pick->bookmark_id);
        refresh_toc_pane();
        update_bar_buttons();
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
    tts_bar_.show(chrome);
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

void MainWnd::apply_chrome()
{
    if (HMENU menu = GetMenu(hwnd_)) {
        for (const int id : {IDM_READER_TOC, IDM_READER_BOOKMARK, IDM_READER_GOTO})
            EnableMenuItem(menu, static_cast<UINT>(id), MF_BYCOMMAND | (in_reader_ ? MF_ENABLED : MF_GRAYED));
    }
    apply_reader_chrome(hwnd_, in_reader_ ? std::optional<uint32_t>(reader_.colors().bg) : std::nullopt);
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
