#include "Covers.h"

#include "base/Handle.h"
#include "base/Log.h"

#include <commctrl.h>

#include <array>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string_view>

namespace eb {
namespace {

constexpr int kCellW = 96, kCellH = 144;

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

} // namespace

int Covers::image_for(const BookRow& row)
{
    if (const auto slot = lru_.touch(row.id); slot && *slot < ImageList_GetImageCount(list_.get()))
        return *slot;
    return style_index(row.fmt);
}

bool Covers::set_cover(const std::string& id, const std::vector<uint32_t>& pixels)
{
    const size_t want = static_cast<size_t>(cell_w_) * static_cast<size_t>(cell_h_);
    if (!list_ || pixels.size() != want)
        return false;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = cell_w_;
    bi.bmiHeader.biHeight = -cell_h_;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    const UniqueGdi bmp(CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0));
    if (!bmp || bits == nullptr)
        return false;
    std::memcpy(bits, pixels.data(), want * sizeof(uint32_t));
    const auto slot = lru_.acquire(id);
    if (!slot)
        return false;
    const auto hbmp = static_cast<HBITMAP>(bmp.get());
    const int count = ImageList_GetImageCount(list_.get());
    if (*slot == count)
        return ImageList_Add(list_.get(), hbmp, nullptr) >= 0;
    return *slot < count && ImageList_Replace(list_.get(), *slot, hbmp, nullptr) != FALSE;
}

bool Covers::rebuild(UINT dpi, bool force)
{
    if (!force && dpi == dpi_ && list_)
        return true;
    const int w = MulDiv(kCellW, static_cast<int>(dpi), 96), h = MulDiv(kCellH, static_cast<int>(dpi), 96);
    const size_t per_cover = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    const size_t max_covers = std::clamp(kBudgetBytes / per_cover, kMinCovers, kMaxCovers);
    UniqueImageList list(ImageList_Create(w, h, ILC_COLOR32, static_cast<int>(kStyles.size() + max_covers), 0));
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
    lru_.reset(max_covers, static_cast<int>(kStyles.size()));
    log::info("cover image list: {}x{} px cells, {} placeholders + {} covers = {} KiB at most", w, h, kStyles.size(), max_covers,
        (kStyles.size() + max_covers) * per_cover / 1024);
    return true;
}

}
