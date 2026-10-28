#include "ReaderSettings.h"

#include "base/Handle.h"
#include "base/Log.h"
#include "base/Text.h"
#include "eb_mupdf.h"
#include "resource.h"

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>

#include <algorithm>
#include <array>
#include <format>
#include <string>
#include <vector>

namespace eb {
namespace {

constexpr DWORD kDwmUseImmersiveDarkMode = 20;
constexpr int kMaxFamilies = 512;
constexpr UINT_PTR kPreviewTimer = 1;
constexpr UINT kPreviewDebounceMs = 300;
constexpr int kLineStepPct = 10;
constexpr std::size_t kMaxFamilyChars = 128;

struct BrushTraits {
    using Raw = HBRUSH;
    static bool valid(HBRUSH b) noexcept { return b != nullptr; }
    static void close(HBRUSH b) noexcept { DeleteObject(b); }
};
using UniqueBrush = Unique<BrushTraits>;

constexpr std::array<int, 4> kSwatches{IDC_SWATCH_BG, IDC_SWATCH_FG, IDC_SWATCH_LINK, IDC_SWATCH_HL};
constexpr std::array<int, 4> kPickers{IDC_PICK_BG, IDC_PICK_FG, IDC_PICK_LINK, IDC_PICK_HL};
constexpr std::array<int, 4> kThemeRadios{IDC_THEME_WHITE, IDC_THEME_BLACK, IDC_THEME_PAPER, IDC_THEME_CUSTOM};
constexpr std::array<int, 3> kFontRadios{IDC_FONT_BOOK, IDC_FONT_FAMILY_ONLY, IDC_FONT_ALL};

struct DlgState {
    ReaderPrefs prefs;
    const PrefsApply* live = nullptr;
    std::array<UniqueBrush, 4> brushes;
    std::array<COLORREF, 16> custom_colors{};
    bool loading = true;
    bool accepted = false;
};

COLORREF to_ref(uint32_t rgb) noexcept
{
    return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

uint32_t from_ref(COLORREF c) noexcept
{
    return (static_cast<uint32_t>(GetRValue(c)) << 16) | (static_cast<uint32_t>(GetGValue(c)) << 8) | GetBValue(c);
}

std::array<uint32_t*, 4> fields(ThemeColors& c) noexcept
{
    return {&c.bg, &c.fg, &c.link, &c.highlight};
}

std::vector<std::string> available_families()
{
    std::vector<std::string> out{"serif", "sans-serif"};
    char** names = nullptr;
    int32_t n = 0;
    if (eb_font_families(&names, &n) != EB_OK)
        return out;
    for (int32_t i = 0; i < n && i < kMaxFamilies; ++i) {
        if (names[i] != nullptr && valid_family(names[i]))
            out.emplace_back(names[i]);
    }
    eb_font_families_free(names, n);
    return out;
}

std::wstring wide(const std::string& s)
{
    return utf8_to_wide(s).value_or(std::wstring{});
}

void fill_family_combo(HWND dlg, const std::string& current)
{
    HWND box = GetDlgItem(dlg, IDC_FONT_FAMILY);
    std::vector<std::string> list = available_families();
    if (std::ranges::find(list, current) == list.end())
        list.push_back(current);
    for (const std::string& f : list)
        SendMessageW(box, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide(f).c_str()));
    const std::wstring cur = wide(current);
    const LRESULT at = SendMessageW(box, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(cur.c_str()));
    SendMessageW(box, CB_SETCURSEL, at == CB_ERR ? 0 : static_cast<WPARAM>(at), 0);
}

void fill_line_combo(HWND dlg)
{
    HWND box = GetDlgItem(dlg, IDC_FONT_LINE);
    for (int pct = kMinLinePct; pct <= kMaxLinePct; pct += kLineStepPct) {
        const std::wstring t = std::format(L"{:.1f}", pct / 100.0);
        SendMessageW(box, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t.c_str()));
    }
}

void update_enabled(HWND dlg, const DlgState& st)
{
    const bool all = st.prefs.font.mode == FontMode::All;
    EnableWindow(GetDlgItem(dlg, IDC_FONT_FAMILY), st.prefs.font.mode != FontMode::Book);
    for (const int id : {IDC_FONT_LINE, IDC_FONT_MARGIN, IDC_FONT_MARGIN_SPIN})
        EnableWindow(GetDlgItem(dlg, id), all);
}

void refresh_swatches(HWND dlg, DlgState& st)
{
    ThemeColors c = resolve_theme(st.prefs.theme);
    const auto f = fields(c);
    for (std::size_t i = 0; i < f.size(); ++i) {
        // cppcheck-suppress objectIndex ; fields() points at distinct members of c
        st.brushes[i].reset(CreateSolidBrush(to_ref(*f[i])));
        InvalidateRect(GetDlgItem(dlg, kSwatches[i]), nullptr, TRUE);
    }
}

void fill_controls(HWND dlg, DlgState& st)
{
    st.loading = true;
    const ReaderPrefs& p = st.prefs;
    CheckRadioButton(dlg, IDC_THEME_WHITE, IDC_THEME_CUSTOM, kThemeRadios[static_cast<std::size_t>(p.theme.id)]);
    CheckRadioButton(dlg, IDC_FONT_BOOK, IDC_FONT_ALL, kFontRadios[static_cast<std::size_t>(p.font.mode)]);
    CheckDlgButton(dlg, IDC_KEEP_IMAGES, p.theme.keep_images ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemInt(dlg, IDC_FONT_SIZE, static_cast<UINT>(p.font.size_pt), FALSE);
    SetDlgItemInt(dlg, IDC_FONT_MARGIN, static_cast<UINT>(p.font.margin_pt), FALSE);
    const int line = (p.font.line_height_pct - kMinLinePct + kLineStepPct / 2) / kLineStepPct;
    SendDlgItemMessageW(dlg, IDC_FONT_LINE, CB_SETCURSEL, static_cast<WPARAM>(line), 0);
    refresh_swatches(dlg, st);
    update_enabled(dlg, st);
    st.loading = false;
}

int read_int(HWND dlg, int id, int fallback) noexcept
{
    BOOL ok = FALSE;
    const UINT v = GetDlgItemInt(dlg, id, &ok, FALSE);
    return ok != FALSE && v < 1000U ? static_cast<int>(v) : fallback;
}

void read_controls(HWND dlg, DlgState& st)
{
    ReaderPrefs& p = st.prefs;
    for (std::size_t i = 0; i < kThemeRadios.size(); ++i) {
        if (IsDlgButtonChecked(dlg, kThemeRadios[i]) == BST_CHECKED)
            p.theme.id = static_cast<ThemeId>(i);
    }
    for (std::size_t i = 0; i < kFontRadios.size(); ++i) {
        if (IsDlgButtonChecked(dlg, kFontRadios[i]) == BST_CHECKED)
            p.font.mode = static_cast<FontMode>(i);
    }
    p.theme.keep_images = IsDlgButtonChecked(dlg, IDC_KEEP_IMAGES) == BST_CHECKED;
    p.font.size_pt = read_int(dlg, IDC_FONT_SIZE, p.font.size_pt);
    p.font.margin_pt = read_int(dlg, IDC_FONT_MARGIN, p.font.margin_pt);
    const LRESULT line = SendDlgItemMessageW(dlg, IDC_FONT_LINE, CB_GETCURSEL, 0, 0);
    if (line != CB_ERR)
        p.font.line_height_pct = kMinLinePct + static_cast<int>(line) * kLineStepPct;
    std::array<wchar_t, kMaxFamilyChars> buf{};
    if (GetDlgItemTextW(dlg, IDC_FONT_FAMILY, buf.data(), static_cast<int>(buf.size())) > 0) {
        if (const auto u = wide_to_utf8(buf.data()); u && valid_family(*u))
            p.font.family = *u;
    }
    p.font = clamped(std::move(p.font));
}

void apply_live(HWND dlg, DlgState& st)
{
    KillTimer(dlg, kPreviewTimer);
    if (st.live != nullptr)
        (*st.live)(st.prefs);
}

// Theme and mode clicks are cheap and apply at once; typing and combo changes wait for a pause, because each one relays out the book.
void changed(HWND dlg, DlgState& st, bool immediate)
{
    if (st.loading)
        return;
    read_controls(dlg, st);
    refresh_swatches(dlg, st);
    update_enabled(dlg, st);
    if (immediate)
        apply_live(dlg, st);
    else
        SetTimer(dlg, kPreviewTimer, kPreviewDebounceMs, nullptr);
}

void pick_color(HWND dlg, DlgState& st, std::size_t which)
{
    ThemeColors c = resolve_theme(st.prefs.theme);
    // cppcheck-suppress objectIndex ; fields() points at distinct members of c
    uint32_t& slot = *fields(c)[which];
    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = dlg;
    // cppcheck-suppress danglingTempReference ; c outlives slot
    cc.rgbResult = to_ref(slot);
    cc.lpCustColors = st.custom_colors.data();
    cc.Flags = CC_RGBINIT | CC_FULLOPEN;
    if (!ChooseColorW(&cc))
        return;
    // cppcheck-suppress danglingTempReference ; c outlives slot
    slot = from_ref(cc.rgbResult);
    st.prefs.theme.id = ThemeId::Custom;
    st.prefs.theme.custom = c;
    fill_controls(dlg, st);
    apply_live(dlg, st);
}

bool is_change_notification(int id, int code) noexcept
{
    if (code == BN_CLICKED)
        return id == IDC_KEEP_IMAGES || std::ranges::find(kThemeRadios, id) != kThemeRadios.end()
            || std::ranges::find(kFontRadios, id) != kFontRadios.end();
    if (code == CBN_SELCHANGE)
        return id == IDC_FONT_FAMILY || id == IDC_FONT_LINE;
    return code == EN_CHANGE && (id == IDC_FONT_SIZE || id == IDC_FONT_MARGIN);
}

INT_PTR on_command(HWND dlg, DlgState& st, WPARAM wp)
{
    const int id = LOWORD(wp), code = HIWORD(wp);
    if (const auto it = std::ranges::find(kPickers, id); it != kPickers.end() && code == BN_CLICKED) {
        pick_color(dlg, st, static_cast<std::size_t>(it - kPickers.begin()));
        return TRUE;
    }
    if (id == IDOK || id == IDCANCEL) {
        KillTimer(dlg, kPreviewTimer);
        if (id == IDOK) {
            read_controls(dlg, st);
            st.accepted = true;
        }
        EndDialog(dlg, id);
        return TRUE;
    }
    if (is_change_notification(id, code)) {
        changed(dlg, st, code == BN_CLICKED);
        return TRUE;
    }
    return FALSE;
}

void init_dialog(HWND dlg, DlgState& st)
{
    for (const int id : {IDC_FONT_SIZE_SPIN, IDC_FONT_MARGIN_SPIN}) {
        const bool size = id == IDC_FONT_SIZE_SPIN;
        SendDlgItemMessageW(dlg, id, UDM_SETRANGE32, size ? kMinSizePt : 0, size ? kMaxSizePt : kMaxMarginPt);
    }
    fill_family_combo(dlg, st.prefs.font.family);
    fill_line_combo(dlg);
    fill_controls(dlg, st);
}

INT_PTR on_color_static(DlgState& st, HDC hdc, HWND ctl) noexcept
{
    const auto it = std::ranges::find(kSwatches, GetDlgCtrlID(ctl));
    if (it == kSwatches.end())
        return FALSE;
    ThemeColors c = resolve_theme(st.prefs.theme);
    // cppcheck-suppress objectIndex ; fields() points at distinct members of c
    SetBkColor(hdc, to_ref(*fields(c)[static_cast<std::size_t>(it - kSwatches.begin())]));
    return reinterpret_cast<INT_PTR>(st.brushes[static_cast<std::size_t>(it - kSwatches.begin())].get());
}

INT_PTR CALLBACK dialog_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) noexcept
{
    try {
        if (msg == WM_INITDIALOG) {
            SetWindowLongPtrW(dlg, DWLP_USER, lp);
            init_dialog(dlg, *reinterpret_cast<DlgState*>(lp)); // NOLINT(performance-no-int-to-ptr): Win32 dialog ABI
            return TRUE;
        }
        auto* st = reinterpret_cast<DlgState*>(GetWindowLongPtrW(dlg, DWLP_USER)); // NOLINT(performance-no-int-to-ptr): Win32 dialog ABI
        if (st == nullptr)
            return FALSE;
        if (msg == WM_COMMAND)
            return on_command(dlg, *st, wp);
        if (msg == WM_TIMER && wp == kPreviewTimer) {
            apply_live(dlg, *st);
            return TRUE;
        }
        if (msg == WM_CTLCOLORSTATIC)
            return on_color_static(*st, reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp)); // NOLINT(performance-no-int-to-ptr): Win32 dialog ABI
    } catch (...) {
        log::error("exception in reader settings dialog");
    }
    return FALSE;
}

}

std::optional<ReaderPrefs> show_reader_settings(HINSTANCE inst, HWND owner, const ReaderPrefs& initial,
    const PrefsApply& live) noexcept
{
    try {
        DlgState st;
        st.prefs = initial;
        st.live = &live;
        DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_READER_SETTINGS), owner, &dialog_proc, reinterpret_cast<LPARAM>(&st));
        return st.accepted ? std::optional<ReaderPrefs>(st.prefs) : std::nullopt;
    } catch (...) {
        log::error("reader settings failed");
        return std::nullopt;
    }
}

void apply_reader_chrome(HWND main, std::optional<uint32_t> bg_rgb) noexcept
{
    const BOOL dark = bg_rgb && wants_dark_title(*bg_rgb) ? TRUE : FALSE;
    DwmSetWindowAttribute(main, kDwmUseImmersiveDarkMode, &dark, sizeof(dark));
}

}
