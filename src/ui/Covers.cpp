#include "Covers.h"

#include "base/Handle.h"

#include <commctrl.h>

#include <array>
#include <cctype>
#include <string_view>

namespace eb {
namespace {

constexpr int kIconW = 72, kIconH = 96;

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

struct ImageListTraits {
    using Raw = HIMAGELIST;
    static bool valid(HIMAGELIST h) noexcept { return h != nullptr; }
    static void close(HIMAGELIST h) noexcept { ImageList_Destroy(h); }
};
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
using UniqueImageList = Unique<ImageListTraits>;
using UniqueGdi = Unique<GdiTraits>;
using UniqueDc = Unique<DcTraits>;

int style_index(std::string_view fmt) noexcept
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

class GenericCovers final : public CoverProvider {
public:
    [[nodiscard]] HIMAGELIST images() const noexcept override { return list_.get(); }
    [[nodiscard]] int image_for(const BookRow& row) const noexcept override { return style_index(row.fmt); }

    bool rebuild(UINT dpi) noexcept override
    {
        const int w = MulDiv(kIconW, static_cast<int>(dpi), 96), h = MulDiv(kIconH, static_cast<int>(dpi), 96);
        UniqueImageList list(ImageList_Create(w, h, ILC_COLOR32, static_cast<int>(kStyles.size()), 0));
        const UniqueDc screen(CreateCompatibleDC(nullptr));
        if (!list || !screen)
            return false;
        const auto sdc = GetDC(nullptr);
        for (const auto& st : kStyles) {
            const UniqueGdi bmp(CreateCompatibleBitmap(sdc, w, h));
            if (!bmp) {
                ReleaseDC(nullptr, sdc);
                return false;
            }
            const auto old = SelectObject(screen.get(), bmp.get());
            draw_book(screen.get(), w, h, st, dpi);
            SelectObject(screen.get(), old);
            ImageList_Add(list.get(), static_cast<HBITMAP>(bmp.get()), nullptr);
        }
        ReleaseDC(nullptr, sdc);
        list_ = std::move(list);
        return true;
    }

private:
    UniqueImageList list_;
};

} // namespace

std::unique_ptr<CoverProvider> make_generic_covers(UINT dpi) noexcept
{
    try {
        auto c = std::make_unique<GenericCovers>();
        if (!c->rebuild(dpi))
            return nullptr;
        return c;
    } catch (...) {
        return nullptr;
    }
}

}
