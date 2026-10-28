#include "ReaderView.h"

#include <windowsx.h>

#include <algorithm>
#include <cstdlib>

namespace eb {
namespace {

constexpr int kDragSlop = 5;
constexpr int kMaxWheelSteps = 10;

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
            if (moved_)
                return true;
            if (const PageLink* link = link_at_point(pt))
                go_to_mark(link->target);
            else
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

const PageLink* ReaderView::link_at_point(POINT pt) const
{
    if (!shown_ || links_page_ != page_ || scale_ <= 0)
        return nullptr;
    const SIZE c = content_size(), v = client_size();
    const auto x = static_cast<float>(pt.x - axis_origin(v.cx, c.cx, pan_x_)) / scale_;
    const auto y = static_cast<float>(pt.y - axis_origin(v.cy, c.cy, pan_y_)) / scale_;
    return link_at(links_, x, y);
}

bool ReaderView::on_set_cursor(LPARAM lp) const
{
    POINT pt{};
    if (LOWORD(lp) != HTCLIENT || !GetCursorPos(&pt) || !ScreenToClient(hwnd_, &pt) || link_at_point(pt) == nullptr)
        return false;
    SetCursor(LoadCursorW(nullptr, IDC_HAND));
    return true;
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

}
