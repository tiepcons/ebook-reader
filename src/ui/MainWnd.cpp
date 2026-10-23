#include "MainWnd.h"

#include "Kv.h"
#include "base/Log.h"

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <string>

namespace eb {
namespace {

constexpr wchar_t kClassName[] = L"EbookReaderMain";
constexpr int kMinW = 640, kMinH = 480;
constexpr int kMargin = 6, kCtlH = 26, kSplitW = 6, kSplitMinDip = 120, kListMinDip = 200, kSplitMaxDip = 2000;

enum : unsigned char { IdSearch = 101, IdSort, IdRescan, IdOpen, IdTree, IdList, IdStatus };

LPARAM to_lp(const void* p) noexcept { return reinterpret_cast<LPARAM>(p); }

template <size_t N>
bool parse_ints(const std::string& s, std::array<int, N>& out) noexcept
{
    const char* p = s.data();
    const char* end = p + s.size();
    for (int& v : out) {
        while (p < end && *p == ' ')
            ++p;
        auto r = std::from_chars(p, end, v);
        if (r.ec != std::errc{})
            return false;
        p = r.ptr;
    }
    return true;
}

bool placement_valid(const WINDOWPLACEMENT& wp) noexcept
{
    const RECT& r = wp.rcNormalPosition;
    if (r.right - r.left < kMinW || r.bottom - r.top < kMinH)
        return false;
    return MonitorFromRect(&r, MONITOR_DEFAULTTONULL) != nullptr;
}

HWND make_child(HWND parent, DWORD ex, const wchar_t* cls, const wchar_t* text, DWORD style, int id) noexcept
{
    return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr); // NOLINT(performance-no-int-to-ptr): Win32 message ABI
}

} // namespace

MainWnd::~MainWnd()
{
    if (hwnd_ != nullptr && IsWindow(hwnd_))
        DestroyWindow(hwnd_);
}

int MainWnd::px(int dip) const noexcept
{
    return MulDiv(dip, static_cast<int>(dpi_), 96);
}

bool MainWnd::create(int show) noexcept
{
    const INITCOMMONCONTROLSEX icc{
        sizeof(icc), ICC_WIN95_CLASSES | ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &MainWnd::wnd_proc;
    wc.hInstance = inst_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = kClassName;
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;
    dpi_ = GetDpiForSystem();
    std::array<int, 1> a{};
    if (const auto v = kv_get(db_, "ui.split"); v && parse_ints(*v, a))
        split_dip_ = std::clamp(a[0], kSplitMinDip, kSplitMaxDip);
    CreateWindowExW(0, kClassName, L"Ebook Reader", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, px(1000),
        px(700), nullptr, nullptr, inst_, this);
    if (hwnd_ == nullptr)
        return false;
    restore_placement(show);
    UpdateWindow(hwnd_);
    return true;
}

void MainWnd::restore_placement(int show)
{
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    std::array<int, 5> a{};
    const auto v = kv_get(db_, "wnd.placement");
    if (v && parse_ints(*v, a)) {
        wp.showCmd = a[0] == SW_SHOWMAXIMIZED ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
        wp.ptMaxPosition = {-1, -1};
        wp.ptMinPosition = {-1, -1};
        wp.rcNormalPosition = {a[1], a[2], a[3], a[4]};
        if (placement_valid(wp) && SetWindowPlacement(hwnd_, &wp))
            return;
    }
    ShowWindow(hwnd_, show);
}

void MainWnd::save_state()
{
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (GetWindowPlacement(hwnd_, &wp)) {
        const RECT& r = wp.rcNormalPosition;
        const int cmd = wp.showCmd == SW_SHOWMAXIMIZED ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
        kv_set(db_, "wnd.placement", std::format("{} {} {} {} {}", cmd, r.left, r.top, r.right, r.bottom));
    }
    kv_set(db_, "ui.split", std::to_string(split_dip_));
}

bool MainWnd::create_children()
{
    search_ = make_child(hwnd_, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IdSearch);
    sort_ = make_child(hwnd_, 0, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IdSort);
    rescan_ = make_child(hwnd_, 0, L"BUTTON", L"Quét lại", BS_PUSHBUTTON | WS_TABSTOP, IdRescan);
    open_ = make_child(hwnd_, 0, L"BUTTON", L"Mở thư mục ebooks", BS_PUSHBUTTON | WS_TABSTOP, IdOpen);
    tree_ = make_child(hwnd_, WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
        TVS_HASLINES | TVS_HASBUTTONS | TVS_LINESATROOT | TVS_SHOWSELALWAYS | WS_TABSTOP | WS_CLIPSIBLINGS, IdTree);
    list_ = make_child(hwnd_, WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        LVS_ICON | LVS_OWNERDATA | LVS_SHAREIMAGELISTS | LVS_SINGLESEL | WS_TABSTOP | WS_CLIPSIBLINGS, IdList);
    status_ = make_child(hwnd_, 0, STATUSCLASSNAMEW, L"Sẵn sàng", SBARS_SIZEGRIP, IdStatus);
    for (HWND h : {search_, sort_, rescan_, open_, tree_, list_, status_}) {
        if (h == nullptr)
            return false;
    }
    SendMessageW(search_, EM_SETCUEBANNER, TRUE, to_lp(L"Tìm sách, tác giả…"));
    for (const wchar_t* s : {L"Tên", L"Tác giả", L"Mới thêm", L"Đọc gần đây"})
        SendMessageW(sort_, CB_ADDSTRING, 0, to_lp(s));
    SendMessageW(sort_, CB_SETCURSEL, 0, 0);
    TVINSERTSTRUCTW ins{};
    ins.hParent = TVI_ROOT;
    ins.hInsertAfter = TVI_LAST;
    ins.item.mask = TVIF_TEXT;
    ins.item.pszText = const_cast<wchar_t*>(L"Tất cả sách");
    if (HTREEITEM root = TreeView_InsertItem(tree_, &ins))
        TreeView_SelectItem(tree_, root);
    ListView_SetItemCountEx(list_, 0, LVSICF_NOINVALIDATEALL);
    dpi_ = GetDpiForWindow(hwnd_);
    update_font();
    return true;
}

void MainWnd::update_font()
{
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi_))
        return;
    UniqueFont nf(CreateFontIndirectW(&ncm.lfMessageFont));
    if (!nf)
        return;
    for (HWND h : {search_, sort_, rescan_, open_, tree_, list_, status_})
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(nf.get()), TRUE);
    font_ = std::move(nf);
}

void MainWnd::layout()
{
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int w = rc.right, h = rc.bottom;
    const int m = px(kMargin), ch = px(kCtlH);
    SendMessageW(status_, WM_SIZE, 0, 0);
    RECT sr{};
    GetWindowRect(status_, &sr);
    const int sh = sr.bottom - sr.top;
    int x = m;
    const auto put = [&](HWND c, int cw, int chh) {
        MoveWindow(c, x, m, cw, chh, TRUE);
        x += cw + m;
    };
    put(search_, px(260), ch);
    put(sort_, px(130), ch + px(200));
    put(rescan_, px(80), ch);
    put(open_, px(150), ch);
    const int top = ch + 2 * m;
    const int body = std::max(0, h - top - sh);
    const int sx = std::clamp(px(split_dip_), px(kSplitMinDip), std::max(px(kSplitMinDip), w - px(kListMinDip)));
    MoveWindow(tree_, 0, top, sx, body, TRUE);
    const int lx = sx + px(kSplitW);
    MoveWindow(list_, lx, top, std::max(0, w - lx), body, TRUE);
}

void MainWnd::splitter_drag(int x_client)
{
    split_dip_ = std::clamp(MulDiv(x_client, 96, static_cast<int>(dpi_)), kSplitMinDip, kSplitMaxDip);
    layout();
}

void MainWnd::open_ebooks_folder()
{
    const auto r = reinterpret_cast<INT_PTR>(
        ShellExecuteW(hwnd_, L"explore", paths_.ebooks.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (r <= 32)
        log::error("open ebooks folder failed ({})", r);
}

void MainWnd::on_command(WPARAM wp)
{
    if (HIWORD(wp) != BN_CLICKED)
        return;
    if (LOWORD(wp) == IdRescan)
        PostMessageW(hwnd_, kMsgRescan, 0, 0);
    else if (LOWORD(wp) == IdOpen)
        open_ebooks_folder();
}

void MainWnd::on_paint()
{
    PAINTSTRUCT ps;
    BeginPaint(hwnd_, &ps);
    EndPaint(hwnd_, &ps);
    if (painted_)
        return;
    painted_ = true;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_);
    log::info("first-paint {} ms", ms.count());
    if (smoke_)
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

bool MainWnd::on_splitter_msg(UINT msg, LPARAM lp, LRESULT& res)
{
    const int sx = px(split_dip_), sw = px(kSplitW);
    res = 0;
    switch (msg) {
    case WM_SETCURSOR: {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd_, &pt);
        if (LOWORD(lp) != HTCLIENT || pt.x < sx || pt.x >= sx + sw)
            return false;
        SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
        res = TRUE;
        return true;
    }
    case WM_LBUTTONDOWN:
        if (const int x = static_cast<short>(LOWORD(lp)); x >= sx && x < sx + sw) {
            dragging_ = true;
            SetCapture(hwnd_);
        }
        return true;
    case WM_MOUSEMOVE:
        if (dragging_)
            splitter_drag(static_cast<short>(LOWORD(lp)) - sw / 2);
        return true;
    case WM_LBUTTONUP:
        if (dragging_)
            ReleaseCapture();
        return true;
    case WM_CAPTURECHANGED:
        dragging_ = false;
        return true;
    default:
        return false;
    }
}

void MainWnd::on_dpi_changed(WPARAM wp, LPARAM lp)
{
    dpi_ = HIWORD(wp);
    const RECT* r = reinterpret_cast<const RECT*>(lp); // NOLINT(performance-no-int-to-ptr): Win32 message ABI
    SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
    update_font();
    layout();
}

LRESULT MainWnd::handle(UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT res = 0;
    if (on_splitter_msg(msg, lp, res))
        return res;
    switch (msg) {
    case WM_CREATE:
        return create_children() ? 0 : -1;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED)
            layout();
        return 0;
    case WM_GETMINMAXINFO: {
        RECT r{0, 0, px(kMinW), px(kMinH)};
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi_);
        reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {r.right - r.left, r.bottom - r.top}; // NOLINT(performance-no-int-to-ptr): Win32 message ABI
        return 0;
    }
    case WM_DPICHANGED:
        on_dpi_changed(wp, lp);
        return 0;
    case WM_COMMAND:
        on_command(wp);
        return 0;
    case kMsgRescan:
        log::info("rescan requested");
        return 0;
    case WM_PAINT:
        on_paint();
        return 0;
    case WM_CLOSE:
        if (!smoke_)
            save_state();
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }
}

LRESULT CALLBACK MainWnd::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) noexcept
{
    MainWnd* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<MainWnd*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); // NOLINT(performance-no-int-to-ptr): Win32 message ABI
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWnd*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)); // NOLINT(performance-no-int-to-ptr): Win32 message ABI
    }
    if (self == nullptr)
        return DefWindowProcW(hwnd, msg, wp, lp);
    LRESULT res = 0;
    try {
        res = self->handle(msg, wp, lp);
    } catch (...) {
        log::error("exception in wnd_proc msg {}", msg);
        res = DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (msg == WM_NCDESTROY) {
        self->hwnd_ = nullptr;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    }
    return res;
}

}
