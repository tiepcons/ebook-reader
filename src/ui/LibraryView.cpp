#include "LibraryView.h"

#include "Kv.h"
#include "UiMessages.h"
#include "base/Log.h"
#include "base/Text.h"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace eb {
namespace {

enum : unsigned char { CmdOpen = 1, CmdReveal, CmdRemove };

std::wstring display_text(const BookRow& r)
{
    std::wstring t = utf8_to_wide(r.title).value_or(std::wstring());
    if (t.empty())
        t = L"(không tên)";
    if (r.status == 2)
        t.insert(0, L"\u26A0 ");
    if (r.percent > 0)
        t += std::format(L" ({}%)", static_cast<int>(std::lround(r.percent * 100)));
    if (r.status == 1)
        t += L" (không tìm thấy)";
    return t;
}

bool file_exists(const std::wstring& path) noexcept
{
    return !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

constexpr size_t kCoversPerMessage = 8;

} // namespace

LibraryView::~LibraryView()
{
    if (worker_)
        worker_->stop();
}

bool LibraryView::attach(HWND list, UINT dpi, const AppPaths& paths)
{
    list_ = list;
    if (!covers_.rebuild(dpi))
        return false;
    ListView_SetImageList(list_, covers_.images(), LVSIL_NORMAL);
    ListView_SetItemCountEx(list_, 0, 0);
    try {
        const auto owner = GetParent(list_);
        cache_ = std::make_unique<CoverCache>(paths.covers);
        worker_ = std::make_unique<CoverWorker>(
            *cache_, paths.data / L"pending_cover", [owner] { return PostMessageW(owner, WM_APP_COVER_READY, 0, 0) != FALSE; });
        if (const auto& bad = worker_->previous_suspect())
            kv_set(db_, "cover.bad." + *bad, "1");
    } catch (...) {
        log::error("cover worker unavailable");
        worker_.reset();
    }
    return true;
}

void LibraryView::request_cover(const BookRow& row)
{
    if (!worker_ || row.status != 0 || covers_.has(row.id) || pending_.contains(row.id) || failed_.contains(row.id))
        return;
    const auto path = book_path(db_, row.id);
    if (!path || kv_get(db_, "cover.bad." + row.id)) {
        failed_.insert(row.id);
        return;
    }
    pending_.insert(row.id);
    if (const auto dropped = worker_->request(row.id, *path))
        pending_.erase(*dropped);
}

void LibraryView::prefetch_covers(size_t n)
{
    for (size_t i = 0; i < std::min(n, rows_.size()); ++i)
        request_cover(rows_[i]);
}

void LibraryView::on_cover_ready()
{
    if (!worker_)
        return;
    const auto done = worker_->take_ready(kCoversPerMessage);
    bool changed = false;
    for (const auto& r : done) {
        pending_.erase(r.id);
        const auto file = r.ok ? cache_->cached(r.id) : std::nullopt;
        if (file && covers_.set_cover(r.id, *file))
            changed = true;
        else
            failed_.insert(r.id);
    }
    if (changed)
        InvalidateRect(list_, nullptr, FALSE);
    if (done.size() == kCoversPerMessage)
        PostMessageW(GetParent(list_), WM_APP_COVER_READY, 0, 0);
}

void LibraryView::rebuild_covers(UINT dpi, bool force)
{
    if (list_ != nullptr && covers_.rebuild(dpi, force)) {
        pending_.clear();
        ListView_SetImageList(list_, covers_.images(), LVSIL_NORMAL);
    }
}

bool LibraryView::refresh(const Query& q)
{
    auto r = query_books(db_, q);
    if (!r) {
        log::error("library query failed (code {})", static_cast<int>(r.error().code));
        return false;
    }
    std::string keep;
    if (const BookRow* cur = selected_row(); cur != nullptr)
        keep = cur->id;
    constexpr UINT kState = LVIS_SELECTED | LVIS_FOCUSED;
    ListView_SetItemState(list_, -1, 0, kState);
    rows_ = std::move(*r);
    text_.assign(rows_.size(), std::wstring());
    ListView_SetItemCountEx(list_, static_cast<int>(rows_.size()), LVSICF_NOSCROLL);
    const auto it = std::ranges::find(rows_, keep, &BookRow::id);
    if (!keep.empty() && it != rows_.end())
        ListView_SetItemState(list_, static_cast<int>(it - rows_.begin()), kState, kState);
    InvalidateRect(list_, nullptr, TRUE);
    return true;
}

const std::wstring& LibraryView::label(size_t i)
{
    if (text_[i].empty())
        text_[i] = display_text(rows_[i]);
    return text_[i];
}

void LibraryView::on_getdispinfo(NMLVDISPINFOW& di)
{
    if (di.item.iItem < 0 || static_cast<size_t>(di.item.iItem) >= rows_.size())
        return;
    const auto i = static_cast<size_t>(di.item.iItem);
    if ((di.item.mask & LVIF_TEXT) != 0 && di.item.pszText != nullptr && di.item.cchTextMax > 0)
        wcsncpy_s(di.item.pszText, static_cast<size_t>(di.item.cchTextMax), label(i).c_str(), _TRUNCATE);
    if ((di.item.mask & LVIF_IMAGE) != 0) {
        request_cover(rows_[i]);
        di.item.iImage = covers_.image_for(rows_[i]);
    }
}

bool LibraryView::on_custom_draw(NMLVCUSTOMDRAW& cd, LRESULT& result) const
{
    if (cd.nmcd.dwDrawStage == CDDS_PREPAINT) {
        result = CDRF_NOTIFYITEMDRAW;
        return true;
    }
    if (cd.nmcd.dwDrawStage != CDDS_ITEMPREPAINT)
        return false;
    const size_t i = cd.nmcd.dwItemSpec;
    result = CDRF_DODEFAULT;
    if (i < rows_.size() && rows_[i].status == 1) {
        cd.clrText = GetSysColor(COLOR_GRAYTEXT);
        result = CDRF_NEWFONT;
    }
    return true;
}

const BookRow* LibraryView::selected_row() const
{
    const int i = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
    return i >= 0 && static_cast<size_t>(i) < rows_.size() ? &rows_[static_cast<size_t>(i)] : nullptr;
}

std::optional<std::string> LibraryView::selected_id() const
{
    const BookRow* r = selected_row();
    return r != nullptr && r->status != 1 ? std::optional<std::string>(r->id) : std::nullopt;
}

void LibraryView::reveal(const BookRow& row) const
{
    const auto path = book_path(db_, row.id);
    if (!path || !file_exists(*path))
        return;
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path->c_str());
    if (pidl == nullptr) {
        log::error("reveal: no shell item for the book");
        return;
    }
    SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
    ILFree(pidl);
}

bool LibraryView::confirm_remove(const BookRow& row) const
{
    const std::wstring content = L"Xóa “" + utf8_to_wide(row.title).value_or(std::wstring()) + L"” khỏi thư viện?";
    TASKDIALOGCONFIG cfg{};
    cfg.cbSize = sizeof(cfg);
    cfg.hwndParent = GetParent(list_);
    cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    cfg.dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
    cfg.nDefaultButton = IDNO;
    cfg.pszWindowTitle = L"Ebook Reader";
    cfg.pszMainIcon = TD_WARNING_ICON;
    cfg.pszMainInstruction = content.c_str();
    cfg.pszContent = L"Tệp sách trên đĩa không bị xóa. Có thể hiện lại sách đã ẩn trong Cài đặt → Thư viện.";
    int button = 0;
    return SUCCEEDED(TaskDialogIndirect(&cfg, &button, nullptr, nullptr)) && button == IDYES;
}

MenuOutcome LibraryView::context_menu(LPARAM lp, std::string& open_id)
{
    POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
    if (lp == -1) {
        const int i = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
        RECT rc{};
        if (i < 0 || !ListView_GetItemRect(list_, i, &rc, LVIR_BOUNDS))
            return MenuOutcome::None;
        pt = {rc.left, rc.bottom};
        ClientToScreen(list_, &pt);
    } else {
        POINT cp = pt;
        ScreenToClient(list_, &cp);
        LVHITTESTINFO ht{};
        ht.pt = cp;
        const int hit = ListView_HitTest(list_, &ht);
        if (hit < 0)
            return MenuOutcome::None;
        ListView_SetItemState(list_, hit, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    const BookRow* row = selected_row();
    if (row == nullptr)
        return MenuOutcome::None;
    const BookRow book = *row;
    const auto menu = CreatePopupMenu();
    if (menu == nullptr)
        return MenuOutcome::None;
    const UINT open_flags = MF_STRING | (book.status == 1 ? MF_GRAYED : 0);
    AppendMenuW(menu, open_flags, CmdOpen, L"Mở");
    AppendMenuW(menu, open_flags, CmdReveal, L"Mở thư mục chứa");
    AppendMenuW(menu, MF_STRING, CmdRemove, L"Xóa khỏi thư viện");
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, GetParent(list_), nullptr);
    DestroyMenu(menu);
    if (cmd == CmdOpen) {
        open_id = book.id;
        return MenuOutcome::Open;
    }
    if (cmd == CmdReveal)
        reveal(book);
    else if (cmd == CmdRemove && confirm_remove(book) && hide_book(db_, book.id))
        return MenuOutcome::Removed;
    return MenuOutcome::None;
}

}
