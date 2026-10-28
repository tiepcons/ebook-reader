#pragma once
#include "eb_mupdf.h"

#include <windows.h>

#include <d2d1.h>
#include <wrl/client.h>

#include <cstdint>
#include <span>

namespace eb {

using BitmapRef = Microsoft::WRL::ComPtr<ID2D1Bitmap>;

// Direct2D surface of the reader window. Device resources are created lazily and rebuilt after D2DERR_RECREATE_TARGET;
// epoch() changes whenever that happens, which invalidates every BitmapRef made before.
class ReaderCanvas {
public:
    bool attach(HWND hwnd) noexcept;
    void resize(int w, int h) noexcept;
    BitmapRef upload(const eb_bitmap& bmp) noexcept;
    // `marks` are translucent rectangles in client pixels (the sentence being read aloud).
    void paint(ID2D1Bitmap* bmp, int x, int y, uint32_t paper_rgb, std::span<const D2D1_RECT_F> marks = {},
        uint32_t mark_rgb = 0) noexcept;
    // Largest bitmap side the device accepts; a conservative default while no device exists.
    [[nodiscard]] uint32_t max_side() noexcept;
    [[nodiscard]] uint32_t epoch() const noexcept { return epoch_; }

private:
    bool ensure_target() noexcept;

    HWND hwnd_ = nullptr;
    Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> rt_;
    uint32_t epoch_ = 0;
};

}
