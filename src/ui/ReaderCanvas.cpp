#include "ReaderCanvas.h"

#include "base/Log.h"

#include <algorithm>

namespace eb {
namespace {

constexpr float kDpi = 96.0F;

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
    if (FAILED(hr))
        log::error("reader: bitmap upload failed (0x{:08x})", static_cast<unsigned>(hr));
    return out;
}

void ReaderCanvas::paint(ID2D1Bitmap* bmp, int x, int y, uint32_t paper_rgb) noexcept
{
    if (!ensure_target())
        return;
    rt_->BeginDraw();
    rt_->Clear(color_of(paper_rgb));
    if (bmp != nullptr) {
        const D2D1_SIZE_U sz = bmp->GetPixelSize();
        const auto fx = static_cast<float>(x), fy = static_cast<float>(y);
        rt_->DrawBitmap(bmp, D2D1::RectF(fx, fy, fx + static_cast<float>(sz.width), fy + static_cast<float>(sz.height)),
            1.0F, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) {
        rt_.Reset();
        ++epoch_;
    }
}

}
