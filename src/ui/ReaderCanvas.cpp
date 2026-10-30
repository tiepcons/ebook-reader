#include "ReaderCanvas.h"

#include "ReaderLogic.h"
#include "base/Log.h"

#include <algorithm>

namespace eb {
namespace {

constexpr float kDpi = 96.0F;
constexpr float kMarkAlpha = 0.45F;

D2D1_COLOR_F color_of(uint32_t rgb) noexcept
{
    constexpr float k = 255.0F;
    return D2D1::ColorF(static_cast<float>((rgb >> 16) & 0xFF) / k, static_cast<float>((rgb >> 8) & 0xFF) / k,
        static_cast<float>(rgb & 0xFF) / k);
}

}

bool ReaderCanvas::attach(HWND hwnd) noexcept
{
    hwnd_ = hwnd;
    return SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.ReleaseAndGetAddressOf()));
}

bool ReaderCanvas::ensure_target() noexcept
{
    if (rt_ || !factory_)
        return rt_ != nullptr;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>(std::max<LONG>(rc.right, 1)),
        static_cast<UINT32>(std::max<LONG>(rc.bottom, 1)));
    const HRESULT hr = factory_->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(hwnd_, size), rt_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        log::error("reader: render target failed (0x{:08x})", static_cast<unsigned>(hr));
        return false;
    }
    rt_->SetDpi(kDpi, kDpi);
    return true;
}

void ReaderCanvas::resize(int w, int h) noexcept
{
    if (rt_ && FAILED(rt_->Resize(D2D1::SizeU(static_cast<UINT32>(std::max(w, 1)), static_cast<UINT32>(std::max(h, 1)))))) {
        rt_.Reset();
        ++epoch_;
    }
}

BitmapRef ReaderCanvas::upload(const eb_bitmap& bmp) noexcept
{
    BitmapRef out;
    if (bmp.bgra == nullptr || bmp.width <= 0 || bmp.height <= 0 || bmp.stride < bmp.width * 4 || !ensure_target())
        return out;
    const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), kDpi, kDpi);
    const HRESULT hr = rt_->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(bmp.width), static_cast<UINT32>(bmp.height)),
        bmp.bgra, static_cast<UINT32>(bmp.stride), props, out.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        log::error("reader: bitmap upload failed (0x{:08x})", static_cast<unsigned>(hr));
        out.Reset();
        if (hr == D2DERR_RECREATE_TARGET) {
            rt_.Reset();
            ++epoch_;
        }
    }
    return out;
}

uint32_t ReaderCanvas::max_side() noexcept
{
    return ensure_target() ? rt_->GetMaximumBitmapSize() : kDefaultMaxSidePx;
}

void ReaderCanvas::paint(std::span<const PageDraw> pages, uint32_t paper_rgb, std::span<const D2D1_RECT_F> marks,
    uint32_t mark_rgb) noexcept
{
    if (!ensure_target())
        return;
    rt_->BeginDraw();
    rt_->Clear(color_of(paper_rgb));
    for (const PageDraw& p : pages) {
        if (p.bmp == nullptr)
            continue;
        const D2D1_SIZE_U sz = p.bmp->GetPixelSize();
        const auto fx = static_cast<float>(p.x), fy = static_cast<float>(p.y);
        rt_->DrawBitmap(p.bmp, D2D1::RectF(fx, fy, fx + static_cast<float>(sz.width), fy + static_cast<float>(sz.height)),
            1.0F, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (!marks.empty() && SUCCEEDED(rt_->CreateSolidColorBrush(color_of(mark_rgb), brush.GetAddressOf()))) {
        brush->SetOpacity(kMarkAlpha);
        for (const D2D1_RECT_F& m : marks)
            rt_->FillRectangle(m, brush.Get());
    }
    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) {
        rt_.Reset();
        ++epoch_;
    }
}

}
