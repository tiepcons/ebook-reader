#include "MainWnd.h"

#include "Kv.h"
#include "LibrarySettings.h"
#include "base/Log.h"

#include <commctrl.h>

#include <format>

namespace eb {
namespace {

constexpr int kMaxSearchChars = 256;

std::wstring with_thousands(size_t n)
{
    std::wstring digits = std::to_wstring(n);
    for (size_t i = digits.size(); i > 3; i -= 3)
        digits.insert(i - 3, 1, L'.');
    return digits;
}

}

bool MainWnd::on_library_msg(UINT msg, WPARAM wp, LPARAM lp, LRESULT& res)
{
    res = 0;
    switch (msg) {
    case kMsgRescan:
        start_scan();
        return true;
    case WM_NOTIFY:
        on_notify(*reinterpret_cast<NMHDR*>(lp), res); // NOLINT(performance-no-int-to-ptr): Win32 message ABI
        return true;
    case WM_CONTEXTMENU:
        if (reinterpret_cast<HWND>(wp) == list_) // NOLINT(performance-no-int-to-ptr): Win32 message ABI
            on_context_menu(lp);
        return true;
    case WM_TIMER:
        if (wp == kSearchTimer) {
            KillTimer(hwnd_, kSearchTimer);
            refresh_list();
        }
        return true;
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE && !smoke_ && painted_
            && std::chrono::steady_clock::now() - last_scan_ >= std::chrono::seconds(30))
            start_scan();
        return true;
    case WM_APP_SCAN_PROGRESS:
        on_scan_progress(wp, lp);
        return true;
    case WM_APP_SCAN_DONE:
        on_scan_done(wp != 0);
        return true;
    case WM_APP_OPEN_BOOK:
        if (!open_queue_.empty()) {
            log::info("open book {}", open_queue_.front());
            open_queue_.erase(open_queue_.begin());
        }
        return true;
    default:
        return false;
    }
}

std::vector<std::filesystem::path> MainWnd::library_roots()
{
    std::vector<std::filesystem::path> roots{paths_.ebooks};
    for (auto& r : load_extra_roots(db_))
        roots.push_back(std::move(r));
    return roots;
}

void MainWnd::start_scan()
{
    if (scan_.running())
        return;
    ScanJob job;
    job.db_file = paths_.data / L"library.db";
    job.marker_file = paths_.data / L"pending_scan";
    job.roots = library_roots();
    job.exclude_dirs = {L"data", L"voices", L"libs", L"fonts"};
    job.meta = stem_meta;
    scan_percent_ = scan_.start(hwnd_, std::move(job)) ? 0 : -1;
    update_status();
    if (scan_percent_ < 0 && smoke_scan_)
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

void MainWnd::on_first_paint()
{
    if (smoke_scan_)
        return;
    if (smoke_) {
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        return;
    }
    refresh_tree();
    refresh_list();
    if (scan_on_start(db_))
        start_scan();
}

void MainWnd::on_scan_progress(WPARAM done, LPARAM total)
{
    if (!scan_.running() || total <= 0)
        return;
    scan_percent_ = static_cast<int>(std::min<LPARAM>(100, static_cast<LPARAM>(done) * 100 / total));
    update_status();
}

void MainWnd::on_scan_done(bool ok)
{
    scan_.finish();
    scan_percent_ = -1;
    last_scan_ = std::chrono::steady_clock::now();
    if (!ok)
        log::error("library scan did not complete");
    refresh_tree();
    refresh_list();
    if (smoke_scan_)
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

void MainWnd::refresh_tree()
{
    const auto folders = list_folders(db_);
    if (!folders) {
        log::error("list_folders failed (code {})", static_cast<int>(folders.error().code));
        return;
    }
    std::vector<std::wstring> roots;
    for (const auto& r : library_roots())
        roots.push_back(r.wstring());
    ftree_.rebuild(build_folder_tree(*folders, roots));
}

Query MainWnd::current_query() const
{
    Query q;
    const int len = std::min(GetWindowTextLengthW(search_), kMaxSearchChars);
    q.text.resize(static_cast<size_t>(len) + 1);
    q.text.resize(static_cast<size_t>(GetWindowTextW(search_, q.text.data(), len + 1)));
    const LRESULT sort = SendMessageW(sort_, CB_GETCURSEL, 0, 0);
    q.sort = sort >= 0 && sort <= static_cast<LRESULT>(SortBy::LastOpened) ? static_cast<SortBy>(sort) : SortBy::Title;
    const TreeScope& scope = ftree_.current();
    q.reading_only = scope.kind == TreeScope::Kind::Reading;
    if (scope.kind == TreeScope::Kind::Folder)
        q.folder = scope.prefix;
    return q;
}

void MainWnd::refresh_list()
{
    lib_.refresh(current_query());
    update_status();
}

void MainWnd::update_status()
{
    std::wstring t = with_thousands(lib_.count()) + L" sách";
    if (scan_percent_ >= 0)
        t += std::format(L" · đang quét {}%", scan_percent_);
    SendMessageW(status_, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(t.c_str()));
}

void MainWnd::request_open(const std::string& id)
{
    open_queue_.push_back(id);
    PostMessageW(hwnd_, WM_APP_OPEN_BOOK, 0, 0);
}

void MainWnd::on_context_menu(LPARAM lp)
{
    std::string id;
    switch (lib_.context_menu(lp, id)) {
    case MenuOutcome::Open:
        request_open(id);
        break;
    case MenuOutcome::Removed:
        refresh_tree();
        refresh_list();
        break;
    default:
        break;
    }
}

void MainWnd::on_notify(NMHDR& hdr, LRESULT& result)
{
    result = 0;
    if (hdr.hwndFrom == tree_ && hdr.code == TVN_SELCHANGEDW) {
        if (ftree_.on_select(reinterpret_cast<const NMTREEVIEWW&>(hdr)))
            refresh_list();
        return;
    }
    if (hdr.hwndFrom != list_)
        return;
    if (hdr.code == LVN_GETDISPINFOW) {
        lib_.on_getdispinfo(reinterpret_cast<NMLVDISPINFOW&>(hdr));
    } else if (hdr.code == NM_CUSTOMDRAW) {
        lib_.on_custom_draw(reinterpret_cast<NMLVCUSTOMDRAW&>(hdr), result);
    } else if (hdr.code == NM_DBLCLK || (hdr.code == LVN_KEYDOWN && reinterpret_cast<const NMLVKEYDOWN&>(hdr).wVKey == VK_RETURN)) {
        if (const auto id = lib_.selected_id())
            request_open(*id);
    }
}

}
