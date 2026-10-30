#include "MainWnd.h"

#include <commctrl.h>

#include <algorithm>
#include <initializer_list>

namespace eb {
namespace {

constexpr int kChapterMaxW = 360, kChapterListRows = 14;
constexpr const wchar_t* kMarkOff = L"☆ Dấu trang";
constexpr const wchar_t* kMarkOn = L"★ Dấu trang";
constexpr int kAaW = 44, kStepBtnW = 28, kTocBtnW = 72, kMarkBtnW = 100;

void add_tip(HWND tips, HWND owner, HWND tool, const wchar_t* text) noexcept
{
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = owner;
    ti.uId = reinterpret_cast<UINT_PTR>(tool);
    ti.lpszText = const_cast<wchar_t*>(text); // NOLINT(cppcoreguidelines-pro-type-const-cast): the tooltip only reads it
    SendMessageW(tips, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
}

}

bool MainWnd::create_bar_buttons()
{
    const auto button = [this](const wchar_t* text, int id) {
        return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 0, 0, hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst_, nullptr); // NOLINT(performance-no-int-to-ptr): Win32 message ABI
    };
    chapter_prev_ = button(L"<", kIdChapterPrev);
    chapter_next_ = button(L">", kIdChapterNext);
    toc_btn_ = button(L"Mục lục", kIdBarToc);
    mark_btn_ = button(kMarkOff, kIdBarBookmark);
    tips_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP, 0, 0, 0, 0, hwnd_, nullptr, inst_, nullptr);
    for (HWND h : {chapter_prev_, chapter_next_, toc_btn_, mark_btn_, tips_}) {
        if (h == nullptr)
            return false;
    }
    add_tip(tips_, hwnd_, chapter_prev_, L"Chương trước");
    add_tip(tips_, hwnd_, chapter_next_, L"Chương sau");
    add_tip(tips_, hwnd_, toc_btn_, L"Mục lục và dấu trang (Ctrl+T)");
    add_tip(tips_, hwnd_, mark_btn_, L"Đặt/bỏ dấu trang (Ctrl+B)");
    return true;
}

void MainWnd::layout_top_bar(const RECT& rc, int left)
{
    const int m = px(kBarMarginDip), ch = px(kBarHeightDip);
    const int aa_x = std::max(0L, rc.right - m - px(kAaW));
    MoveWindow(aa_, aa_x, m, px(kAaW), ch, TRUE);
    const int mark_x = aa_x - m - px(kMarkBtnW);
    const int toc_x = mark_x - m - px(kTocBtnW);
    MoveWindow(mark_btn_, mark_x, m, px(kMarkBtnW), ch, TRUE);
    MoveWindow(toc_btn_, toc_x, m, px(kTocBtnW), ch, TRUE);
    const bool buttons = !fullscreen_ && toc_x >= left;
    ShowWindow(toc_btn_, buttons ? SW_SHOW : SW_HIDE);
    ShowWindow(mark_btn_, buttons ? SW_SHOW : SW_HIDE);
    const int step = px(kStepBtnW);
    const int cw = std::min(px(kChapterMaxW), toc_x - m - left - 2 * (step + m));
    const bool chapters = !fullscreen_ && chapter_count_ >= 2 && cw > px(kAaW);
    MoveWindow(chapter_prev_, left, m, step, ch, TRUE);
    MoveWindow(chapter_combo_, left + step + m, m, std::max(cw, 0), ch * kChapterListRows, TRUE);
    MoveWindow(chapter_next_, left + step + m + std::max(cw, 0) + m, m, step, ch, TRUE);
    for (HWND h : {chapter_prev_, chapter_combo_, chapter_next_})
        ShowWindow(h, chapters ? SW_SHOW : SW_HIDE);
}

void MainWnd::update_bar_buttons()
{
    const std::span<const TocItem> toc = reader_.toc();
    const uint64_t mark = reader_.mark();
    const bool known = mark != kNoAnchor;
    const int cur = remembered_toc_index();
    EnableWindow(chapter_prev_, known && toc_prev_index(toc, mark, cur) >= 0 ? TRUE : FALSE);
    EnableWindow(chapter_next_, known && toc_next_index(toc, mark, cur) >= 0 ? TRUE : FALSE);
    if (const bool marked = reader_.page_bookmarked(); marked != bar_marked_) {
        bar_marked_ = marked;
        SetWindowTextW(mark_btn_, marked ? kMarkOn : kMarkOff);
    }
}

int MainWnd::remembered_toc_index() const noexcept
{
    return nav_idx_ >= 0 && !nav_pending_ && reader_.page() == nav_page_ ? nav_idx_ : -1;
}

void MainWnd::jump_to_toc(int idx)
{
    const std::vector<TocItem>& toc = reader_.toc();
    if (idx < 0 || static_cast<std::size_t>(idx) >= toc.size())
        return;
    nav_idx_ = idx;
    nav_pending_ = true;
    jump_to_mark(toc[static_cast<std::size_t>(idx)].target);
}

void MainWnd::reset_bar_marked()
{
    bar_marked_ = false;
    SetWindowTextW(mark_btn_, kMarkOff);
}

bool MainWnd::on_bar_command(int id)
{
    if (id == kIdBarToc) {
        on_shortcut_event(ReaderEvent::Toc);
    } else if (id == kIdBarBookmark) {
        on_shortcut_event(ReaderEvent::Bookmark);
        SetFocus(reader_.hwnd());
    } else if (id == kIdChapterPrev || id == kIdChapterNext) {
        const std::span<const TocItem> toc = reader_.toc();
        const int cur = remembered_toc_index();
        const int idx = id == kIdChapterNext ? toc_next_index(toc, reader_.mark(), cur) : toc_prev_index(toc, reader_.mark(), cur);
        if (idx >= 0 && reader_.mark() != kNoAnchor)
            jump_to_toc(idx);
        SetFocus(reader_.hwnd());
    } else {
        return false;
    }
    return true;
}

}
