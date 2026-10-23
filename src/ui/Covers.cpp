#include "Covers.h"

#include "base/Handle.h"

#include <commctrl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <array>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace eb {
namespace {

constexpr int kCellW = 96, kCellH = 144;
constexpr uint32_t kCoverBg = 0xFFEBEBEB;
constexpr UINT kMaxSidePx = 16384;

struct FormatStyle {
    std::string_view fmt;
    const wchar_t* label;
    COLORREF color;
};

constexpr std::array<FormatStyle, 8> kStyles = {{
    {"epub", L"EPUB", RGB(46, 125, 90)},
    {"pdf", L"PDF", RGB(190, 60, 55)},
    {"mobi", L"MOBI", RGB(190, 120, 40)},
    {"azw3", L"AZW3", RGB(200, 150, 40)},
    {"fb2", L"FB2", RGB(110, 80, 160)},
    {"txt", L"TXT", RGB(90, 100, 115)},
    {"cbz", L"CBZ", RGB(40, 120, 170)},
    {"", L"?", RGB(120, 120, 120)},
}};

struct GdiTraits {
    using Raw = HGDIOBJ;
    static bool valid(HGDIOBJ h) noexcept { return h != nullptr; }
    static void close(HGDIOBJ h) noexcept { DeleteObject(h); }
};
struct DcTraits {
    using Raw = HDC;
    static bool valid(HDC h) noexcept { return h != nullptr; }
    static void close(HDC h) noexcept { DeleteDC(h); }
};
using UniqueGdi = Unique<GdiTraits>;
using UniqueDc = Unique<DcTraits>;

int style_index(const std::string& fmt) noexcept
{
    for (size_t i = 0; i + 1 < kStyles.size(); ++i) {
        if (fmt.size() == kStyles[i].fmt.size()) {
            bool same = true;
            for (size_t k = 0; k < fmt.size(); ++k)
                same = same && std::tolower(static_cast<unsigned char>(fmt[k])) == kStyles[i].fmt[k];
            if (same)
                return static_cast<int>(i);
        }
    }
    return static_cast<int>(kStyles.size()) - 1;
}

void draw_book(HDC dc, int w, int h, const FormatStyle& st, UINT dpi)
{
    const COLORREF bg = GetSysColor(COLOR_WINDOW);
    const UniqueGdi bg_brush(CreateSolidBrush(bg));
    const RECT all{0, 0, w, h};
    FillRect(dc, &all, static_cast<HBRUSH>(bg_brush.get()));
    const int pad = MulDiv(4, static_cast<int>(dpi), 96);
    const UniqueGdi brush(CreateSolidBrush(st.color));
    const UniqueGdi pen(CreatePen(PS_SOLID, 1, RGB(255, 255, 255)));
    const auto old_b = SelectObject(dc, brush.get());
    const auto old_p = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, pad, pad, w - pad, h - pad, pad * 2, pad * 2);
    SelectObject(dc, pen.get());
    MoveToEx(dc, pad * 3, pad, nullptr);
    LineTo(dc, pad * 3, h - pad);
    SelectObject(dc, old_p);
    SelectObject(dc, old_b);
    const UniqueGdi font(CreateFontW(-MulDiv(13, static_cast<int>(dpi), 96), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI"));
    const auto old_f = SelectObject(dc, font.get());
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT tr{pad * 3, pad, w - pad, h - pad};
    DrawTextW(dc, st.label, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old_f);
}

using Microsoft::WRL::ComPtr;

UniqueGdi make_dib(int w, int h, uint32_t** bits)
{
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* p = nullptr;
    UniqueGdi bmp(CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &p, nullptr, 0));
    *bits = static_cast<uint32_t*>(p);
    return bmp;
}

ComPtr<IWICBitmapSource> scaled_frame(IWICImagingFactory* f, const std::filesystem::path& file, int w, int h, int* nw, int* nh)
{
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    UINT sw = 0, sh = 0;
    if (FAILED(f->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))
        || FAILED(dec->GetFrame(0, &frame)) || FAILED(frame->GetSize(&sw, &sh)) || sw == 0 || sh == 0 || sw > kMaxSidePx
        || sh > kMaxSidePx)
        return nullptr;
    const double s = std::min(static_cast<double>(w) / sw, static_cast<double>(h) / sh);
    *nw = std::max(1, static_cast<int>(std::lround(sw * s)));
    *nh = std::max(1, static_cast<int>(std::lround(sh * s)));
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> conv;
    ComPtr<IWICBitmapSource> out;
    if (FAILED(f->CreateBitmapScaler(&scaler))
        || FAILED(scaler->Initialize(frame.Get(), static_cast<UINT>(*nw), static_cast<UINT>(*nh),
            WICBitmapInterpolationModeHighQualityCubic))
        || FAILED(f->CreateFormatConverter(&conv))
        || FAILED(conv->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeCustom)))
        return nullptr;
    conv.As(&out);
    return out;
}

UniqueGdi load_cover_bitmap(const std::filesystem::path& file, int w, int h)
{
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))))
        return {};
    int nw = 0, nh = 0;
    const auto src = scaled_frame(f.Get(), file, w, h, &nw, &nh);
    uint32_t* bits = nullptr;
    UniqueGdi dib = make_dib(w, h, &bits);
    if (!src || !dib || bits == nullptr)
        return {};
    std::fill_n(bits, static_cast<size_t>(w) * static_cast<size_t>(h), kCoverBg);
    const UINT stride = static_cast<UINT>(w) * 4;
    const size_t offset = static_cast<size_t>((h - nh) / 2) * stride + static_cast<size_t>((w - nw) / 2) * 4;
    const UINT room = stride * static_cast<UINT>(h) - static_cast<UINT>(offset);
    auto* dst = reinterpret_cast<BYTE*>(bits) + offset;
    if (FAILED(src->CopyPixels(nullptr, stride, room, dst)))
        return {};
    return dib;
}

} // namespace

int Covers::image_for(const BookRow& row)
{
    if (const auto it = map_.find(row.id); it != map_.end()) {
        lru_.splice(lru_.begin(), lru_, it->second);
        return it->second->slot;
    }
    return style_index(row.fmt);
}

bool Covers::set_cover(const std::string& id, const std::filesystem::path& jpeg)
{
    if (!list_)
        return false;
    const UniqueGdi bmp = load_cover_bitmap(jpeg, cell_w_, cell_h_);
    if (!bmp)
        return false;
    const auto hbmp = static_cast<HBITMAP>(bmp.get());
    if (const auto it = map_.find(id); it != map_.end()) {
        lru_.splice(lru_.begin(), lru_, it->second);
        return ImageList_Replace(list_.get(), it->second->slot, hbmp, nullptr) != FALSE;
    }
    if (lru_.size() >= kMaxCovers) {
        const int slot = lru_.back().slot;
        if (ImageList_Replace(list_.get(), slot, hbmp, nullptr) == FALSE)
            return false;
        map_.erase(lru_.back().id);
        lru_.pop_back();
        lru_.push_front({id, slot});
    } else {
        const int slot = ImageList_Add(list_.get(), hbmp, nullptr);
        if (slot < 0)
            return false;
        lru_.push_front({id, slot});
    }
    map_[id] = lru_.begin();
    return true;
}

bool Covers::rebuild(UINT dpi, bool force)
{
    if (!force && dpi == dpi_ && list_)
        return true;
    const int w = MulDiv(kCellW, static_cast<int>(dpi), 96), h = MulDiv(kCellH, static_cast<int>(dpi), 96);
    UniqueImageList list(ImageList_Create(w, h, ILC_COLOR32, static_cast<int>(kStyles.size()), 16));
    const auto screen = GetDC(nullptr);
    if (!list || screen == nullptr)
        return false;
    const UniqueDc mem(CreateCompatibleDC(screen));
    bool ok = mem.valid();
    for (size_t i = 0; ok && i < kStyles.size(); ++i) {
        const UniqueGdi bmp(CreateCompatibleBitmap(screen, w, h));
        ok = bmp.valid();
        if (!ok)
            break;
        const auto old = SelectObject(mem.get(), bmp.get());
        draw_book(mem.get(), w, h, kStyles[i], dpi);
        SelectObject(mem.get(), old);
        ok = ImageList_Add(list.get(), static_cast<HBITMAP>(bmp.get()), nullptr) >= 0;
    }
    ReleaseDC(nullptr, screen);
    if (!ok)
        return false;
    list_ = std::move(list);
    dpi_ = dpi;
    cell_w_ = w;
    cell_h_ = h;
    lru_.clear();
    map_.clear();
    return true;
}

}
