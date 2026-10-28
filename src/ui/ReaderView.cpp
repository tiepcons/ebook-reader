#include "ReaderView.h"

#include "base/Log.h"
#include "base/Text.h"

#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>

namespace eb {
namespace {

constexpr wchar_t kClass[] = L"EbookReaderView";
constexpr UINT_PTR kResizeTimer = 1;
constexpr UINT kResizeDebounceMs = 150;
constexpr int kDragSlop = 5;
constexpr int kPanEnd = 1 << 28;
constexpr int kMaxWheelSteps = 10;
constexpr int64_t kFlushEveryMs = 30000;

int64_t steady_ms() noexcept
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

int64_t unix_now() noexcept
{
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::wstring wide(std::string_view s)
{
    return utf8_to_wide(s).value_or(std::wstring{});
}

std::wstring page_error_text(eb_status st)
{
    if (st == EB_E_NOMEM)
        return L"Không đủ bộ nhớ để hiển thị trang này";
    if (st == EB_E_LIMIT)
        return L"Trang quá lớn để hiển thị";
    return L"Lỗi trang: " + wide(eb_status_text(st));
}

}

ReaderView::~ReaderView()
{
    close();
    if (hwnd_ != nullptr && IsWindow(hwnd_))
        DestroyWindow(hwnd_);
}

bool ReaderView::create(HINSTANCE inst, HWND parent) noexcept
{
    parent_ = parent;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &ReaderView::wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;
    CreateWindowExW(0, kClass, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent, nullptr, inst, this);
    return hwnd_ != nullptr && canvas_.attach(hwnd_);
}

LRESULT CALLBACK ReaderView::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) noexcept
{
    ReaderView* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<ReaderView*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); // NOLINT(performance-no-int-to-ptr): Win32 message ABI
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<ReaderView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)); // NOLINT(performance-no-int-to-ptr): Win32 message ABI
    }
    if (self == nullptr)
        return DefWindowProcW(hwnd, msg, wp, lp);
    LRESULT res = 0;
    try {
        res = self->handle(msg, wp, lp);
    } catch (...) {
        log::error("exception in reader wnd_proc msg {}", msg);
        res = DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (msg == WM_NCDESTROY) {
        self->hwnd_ = nullptr;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    }
    return res;
}

LRESULT ReaderView::handle(UINT msg, WPARAM wp, LPARAM lp)
{
    if (on_mouse(msg, wp, lp))
        return 0;
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        on_paint();
        return 0;
    case WM_SIZE:
        canvas_.resize(LOWORD(lp), HIWORD(lp));
        SetTimer(hwnd_, kResizeTimer, kResizeDebounceMs, nullptr);
        return 0;
    case WM_DPICHANGED_AFTERPARENT:
        SetTimer(hwnd_, kResizeTimer, kResizeDebounceMs, nullptr);
        return 0;
    case WM_TIMER:
        if (wp != kResizeTimer)
            return 0;
        KillTimer(hwnd_, kResizeTimer);
        apply_view();
        return 0;
    case WM_APP_PAGE_READY:
        on_results();
        return 0;
    case WM_KEYDOWN:
        on_action(nav_for_key(static_cast<unsigned>(wp), GetKeyState(VK_SHIFT) < 0, GetKeyState(VK_CONTROL) < 0));
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd_, kResizeTimer);
        close();
        return 0;
    default:
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }
}

bool ReaderView::on_mouse(UINT msg, WPARAM wp, LPARAM lp)
{
    const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    switch (msg) {
    case WM_LBUTTONDOWN:
        SetFocus(hwnd_);
        SetCapture(hwnd_);
        dragging_ = true;
        moved_ = false;
        drag_start_ = drag_last_ = pt;
        return true;
    case WM_MOUSEMOVE:
        if (dragging_) {
            moved_ = moved_ || std::abs(pt.x - drag_start_.x) > kDragSlop || std::abs(pt.y - drag_start_.y) > kDragSlop;
            if (moved_)
                pan_by(drag_last_.x - pt.x, drag_last_.y - pt.y);
            drag_last_ = pt;
        }
        return true;
    case WM_LBUTTONUP:
        if (dragging_) {
            dragging_ = false;
            ReleaseCapture();
            if (!moved_)
                on_action(nav_for_click(pt.x, client_size().cx));
        }
        return true;
    case WM_CAPTURECHANGED:
        dragging_ = false;
        return true;
    case WM_MOUSEWHEEL:
        on_wheel(GET_WHEEL_DELTA_WPARAM(wp), (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) != 0);
        return true;
    default:
        return false;
    }
}

SIZE ReaderView::client_size() const noexcept
{
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    return {rc.right, rc.bottom};
}

SIZE ReaderView::content_size() const noexcept
{
    if (!shown_)
        return {0, 0};
    const D2D1_SIZE_U sz = shown_->GetPixelSize();
    return {static_cast<LONG>(sz.width), static_cast<LONG>(sz.height)};
}

void ReaderView::on_paint()
{
    PAINTSTRUCT ps;
    BeginPaint(hwnd_, &ps);
    const SIZE c = content_size(), v = client_size();
    canvas_.paint(shown_.Get(), axis_origin(v.cx, c.cx, pan_x_), axis_origin(v.cy, c.cy, pan_y_), colors().bg);
    EndPaint(hwnd_, &ps);
    if (canvas_.epoch() != epoch_) {
        check_epoch();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void ReaderView::check_epoch()
{
    if (canvas_.epoch() == epoch_)
        return;
    epoch_ = canvas_.epoch();
    cache_.clear();
    set_shown({});
    ++view_id_;
    send_want();
}

void ReaderView::set_shown(BitmapRef b)
{
    shown_ = std::move(b);
    const SIZE c = content_size();
    cache_.set_cap(kBitmapCacheBytes - std::min(kBitmapCacheBytes, bitmap_bytes(c.cx, c.cy)));
}

ViewParams ReaderView::current_view() const
{
    const SIZE v = client_size();
    ViewParams p;
    p.client_w = v.cx;
    p.client_h = v.cy;
    p.dpi = std::max(GetDpiForWindow(hwnd_), 96U);
    p.zoom = reflowable_ ? ZoomState{} : zoom_;
    return p;
}

void ReaderView::send_want()
{
    if (!worker_)
        return;
    RenderWant w;
    w.view_id = view_id_;
    w.view = view_;
    w.anchor = anchor_;
    w.mark = mark_;
    w.max_side_px = canvas_.max_side();
    w.style = style_;
    const SIZE c = content_size();
    w.prefetch = bitmap_bytes(c.cx, c.cy) <= kPrefetchMaxBytes;
    w.page = page_;
    const bool known = anchor_ == kNoAnchor; // a jump to a bookmark lands on a page the cache cannot be keyed by yet
    w.have_cur = known && cache_.contains(page_);
    w.have_next = known && cache_.contains(page_ + 1);
    w.have_prev = known && cache_.contains(page_ - 1);
    want_gen_ = worker_->request(w);
}

void ReaderView::apply_view()
{
    if (!worker_)
        return;
    const ViewParams v = current_view();
    if (v == view_)
        return;
    view_ = v;
    ++view_id_;
    cache_.clear();
    send_want();
}

bool ReaderView::open(OpenJob job)
{
    close();
    error_.clear();
    if (!prefs_loaded_) {
        prefs_ = load_reader_prefs(db_);
        prefs_loaded_ = true;
    }
    fmt_ = job.fmt;
    book_id_ = job.id;
    rebuild_style();
    saver_.emplace(store_, job.id);
    const auto saved = store_.load(job.id);
    const Progress p = saved ? saved->value_or(Progress{}) : Progress{};
    if (p.mark != 0) {
        anchor_ = p.mark;
        saved_percent_ = p.percent;
    }
    epoch_ = canvas_.epoch();
    view_ = current_view();
    ++view_id_;
    awaiting_first_ = true;
    last_flush_ms_ = steady_ms();
    try {
        worker_ = std::make_unique<RenderWorker>(hwnd_, WM_APP_PAGE_READY, std::move(job));
    } catch (...) {
        log::error("reader: worker start failed");
        close();
        return false;
    }
    send_want();
    return true;
}

void ReaderView::close() noexcept
{
    flush();
    worker_.reset();
    saver_.reset();
    cache_.clear();
    set_shown({});
    zoom_ = ZoomState{};
    anchor_ = mark_ = kNoAnchor;
    page_ = count_ = 0;
    scale_ = 0;
    reflowable_ = awaiting_first_ = false;
    resume_percent_ = -1;
    pan_x_ = pan_y_ = 0;
    wheel_acc_ = 0;
    chapter_.clear();
    note_.clear();
    book_id_.clear();
    toc_.clear();
    content_version_ = 0;
    end_mark_ = UINT64_MAX;
}

void ReaderView::flush() noexcept
{
    if (saver_)
        saver_->flush();
}

void ReaderView::tick() noexcept
{
    if (!saver_)
        return;
    const int64_t now = steady_ms();
    saver_->tick(now);
    if (now - last_flush_ms_ >= kFlushEveryMs) {
        saver_->flush();
        last_flush_ms_ = now;
    }
}

void ReaderView::on_results()
{
    if (!worker_)
        return;
    for (RenderResult& r : worker_->take()) {
        check_epoch();
        if (r.kind == RenderResult::Kind::OpenFailed)
            fail_open(r);
        else if (r.view_id == view_id_)
            store_result(r);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ReaderView::fail_open(const RenderResult& r)
{
    error_ = L"Không mở được sách: " + wide(eb_status_text(r.status));
    if (!r.detail.empty())
        error_ += L" (" + wide(r.detail) + L")";
    notify(ReaderEvent::OpenFailed);
}

void ReaderView::store_result(RenderResult& r)
{
    count_ = r.page_count;
    reflowable_ = r.reflowable;
    if (r.bmp) {
        const std::size_t bytes = bitmap_bytes(r.bmp->width, r.bmp->height);
        if (BitmapRef ref = canvas_.upload(*r.bmp))
            cache_.put(r.page, std::move(ref), bytes);
        else if (r.primary && r.status == EB_OK)
            r.status = EB_E_INTERNAL;
        if (r.primary && r.scale > 0)
            scale_ = r.scale;
        r.bmp.reset();
    }
    if (r.primary && r.generation == want_gen_)
        show_primary(r);
}

void ReaderView::show_primary(const RenderResult& r)
{
    if (r.status != EB_OK) {
        note_ = page_error_text(r.status);
        notify(ReaderEvent::PageSettled);
        return;
    }
    note_.clear();
    if (r.page != page_)
        pan_x_ = pan_y_ = 0;
    page_ = r.page;
    mark_ = r.mark;
    end_mark_ = r.end_mark;
    chapter_ = wide(r.chapter);
    if (toc_.empty() && worker_) {
        toc_ = worker_->toc();
        if (!toc_.empty())
            ++content_version_;
    }
    if (BitmapRef* b = cache_.get(page_))
        set_shown(*b);
    if (awaiting_first_ && anchor_ != kNoAnchor && page_ > 0)
        resume_percent_ = static_cast<int>(std::lround(saved_percent_ * 100));
    awaiting_first_ = false;
    anchor_ = kNoAnchor;
    record_progress(r);
    notify(ReaderEvent::PageSettled);
}

void ReaderView::record_progress(const RenderResult& r) noexcept
{
    if (!saver_)
        return;
    saver_->update(Progress{r.mark, reading_fraction(r.page, r.page_count), -1, unix_now()});
}

void ReaderView::notify(ReaderEvent e) const noexcept
{
    PostMessageW(parent_, WM_APP_READER_EVENT, static_cast<WPARAM>(e), static_cast<LPARAM>(page_));
}

void ReaderView::go_to(int32_t page, bool from_end)
{
    if (count_ <= 0)
        return;
    page = std::clamp(page, 0, count_ - 1);
    if (page == page_)
        return;
    page_ = page;
    anchor_ = kNoAnchor; // an explicit page turn overrides a TOC/bookmark jump still in flight
    pan_x_ = 0;
    pan_y_ = from_end ? kPanEnd : 0;
    mark_ = kNoAnchor;
    if (BitmapRef* b = cache_.get(page_))
        set_shown(*b);
    send_want();
    InvalidateRect(hwnd_, nullptr, FALSE);
    notify(ReaderEvent::PageShown);
}

void ReaderView::go_to_start()
{
    go_to(0);
}

void ReaderView::rebuild_style()
{
    style_ = std::make_shared<const ReaderStyle>(make_style(prefs_, fmt_));
}

void ReaderView::set_prefs(const ReaderPrefs& p, bool persist)
{
    ReaderPrefs n = p;
    n.font = clamped(std::move(n.font));
    if (persist && !save_reader_prefs(db_, n))
        log::error("reader: cannot save theme and font settings");
    prefs_loaded_ = true;
    if (n == prefs_)
        return;
    prefs_ = std::move(n);
    rebuild_style();
    if (hwnd_ != nullptr)
        InvalidateRect(hwnd_, nullptr, FALSE);
    if (!worker_)
        return;
    cache_.clear();
    ++view_id_;
    send_want();
}

void ReaderView::bump_font_size(int direction)
{
    ReaderPrefs n = prefs_;
    n.font.size_pt = std::clamp(n.font.size_pt + direction, kMinSizePt, kMaxSizePt);
    if (n.font.size_pt == prefs_.font.size_pt)
        return;
    set_prefs(n, true);
}

void ReaderView::zoom(int direction)
{
    if (reflowable_) {
        if (direction != 0)
            bump_font_size(direction);
        return;
    }
    if (scale_ <= 0)
        return;
    zoom_ = direction == 0 ? toggle_fit_width(zoom_) : zoom_step(scale_, view_.dpi, direction);
    pan_x_ = pan_y_ = 0;
    apply_view();
}

void ReaderView::on_action(NavAction a)
{
    switch (a) {
    case NavAction::Prev:
        go_to(page_ - 1);
        break;
    case NavAction::Next:
        go_to(page_ + 1);
        break;
    case NavAction::First:
        go_to(0);
        break;
    case NavAction::Last:
        go_to(count_ - 1);
        break;
    case NavAction::ZoomIn:
        zoom(1);
        break;
    case NavAction::ZoomOut:
        zoom(-1);
        break;
    case NavAction::ToggleFitWidth:
        zoom(0);
        break;
    case NavAction::Fullscreen:
        notify(ReaderEvent::Fullscreen);
        break;
    case NavAction::Close:
        notify(ReaderEvent::CloseRequested);
        break;
    case NavAction::Settings:
        notify(ReaderEvent::Settings);
        break;
    case NavAction::Toc:
        notify(ReaderEvent::Toc);
        break;
    case NavAction::Bookmark:
        notify(ReaderEvent::Bookmark);
        break;
    case NavAction::GoTo:
        notify(ReaderEvent::GoTo);
        break;
    case NavAction::None:
        break;
    }
}

void ReaderView::pan_by(int dx, int dy)
{
    const SIZE c = content_size(), v = client_size();
    pan_x_ = clamp_pan(pan_x_ + dx, c.cx, v.cx);
    pan_y_ = clamp_pan(pan_y_ + dy, c.cy, v.cy);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ReaderView::on_wheel(int delta, bool ctrl)
{
    if (ctrl) {
        zoom(delta > 0 ? 1 : -1);
        return;
    }
    wheel_acc_ += delta;
    const int steps = std::clamp(wheel_acc_ / WHEEL_DELTA, -kMaxWheelSteps, kMaxWheelSteps);
    wheel_acc_ -= steps * WHEEL_DELTA;
    for (int i = 0; i < std::abs(steps); ++i)
        wheel_step(steps > 0 ? -1 : 1);
}

void ReaderView::wheel_step(int direction)
{
    const SIZE c = content_size(), v = client_size();
    const int max_pan = static_cast<int>(std::max(0L, c.cy - v.cy));
    if (max_pan == 0 || (direction > 0 && pan_y_ >= max_pan)) {
        go_to(page_ + direction);
        return;
    }
    if (direction < 0 && pan_y_ <= 0) {
        go_to(page_ - 1, true);
        return;
    }
    pan_by(0, direction * std::max(1, static_cast<int>(v.cy / 4)));
}

int ReaderView::take_resume_percent() noexcept
{
    return std::exchange(resume_percent_, -1);
}

}
