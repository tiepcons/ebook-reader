#include "LibrarySettings.h"

#include "Kv.h"
#include "base/Log.h"
#include "base/Text.h"
#include "lib/LibraryModel.h"
#include "resource.h"

#include <commctrl.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>

namespace eb {
namespace {

constexpr size_t kMaxRootChars = 4096;

struct DialogState {
    Db* db;
    std::vector<std::wstring> roots;
    bool scan_start;
    bool changed;
};

std::wstring pick_folder(HWND owner)
{
    Microsoft::WRL::ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return {};
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    if (FAILED(dlg->Show(owner)))
        return {};
    Microsoft::WRL::ComPtr<IShellItem> item;
    if (FAILED(dlg->GetResult(&item)))
        return {};
    PWSTR raw = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)))
        return {};
    const std::wstring path(raw);
    CoTaskMemFree(raw);
    return path.size() <= kMaxRootChars ? path : std::wstring();
}

void fill_list(HWND dlg, const DialogState& st)
{
    const auto list = GetDlgItem(dlg, IDC_ROOT_LIST);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (const auto& r : st.roots)
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(r.c_str()));
}

void add_root(HWND dlg, DialogState& st)
{
    std::wstring p = pick_folder(dlg);
    if (p.empty() || st.roots.size() >= kMaxExtraRoots)
        return;
    const std::wstring key = lower_case(p);
    if (std::ranges::any_of(st.roots, [&](const std::wstring& r) { return lower_case(r) == key; }))
        return;
    st.roots.push_back(std::move(p));
    st.changed = true;
    fill_list(dlg, st);
}

void remove_root(HWND dlg, DialogState& st)
{
    const LRESULT sel = SendDlgItemMessageW(dlg, IDC_ROOT_LIST, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR || static_cast<size_t>(sel) >= st.roots.size())
        return;
    st.roots.erase(st.roots.begin() + sel);
    st.changed = true;
    fill_list(dlg, st);
}

void show_save_error(HWND dlg) noexcept
{
    TASKDIALOGCONFIG cfg{};
    cfg.cbSize = sizeof(cfg);
    cfg.hwndParent = dlg;
    cfg.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    cfg.pszWindowTitle = L"Ebook Reader";
    cfg.pszMainIcon = TD_ERROR_ICON;
    cfg.pszContent = L"Không lưu được cài đặt";
    TaskDialogIndirect(&cfg, nullptr, nullptr, nullptr);
}

bool save(const DialogState& st)
{
    std::string joined;
    for (const auto& r : st.roots) {
        if (auto u = wide_to_utf8(r); u) {
            if (!joined.empty())
                joined.push_back('\n');
            joined += *u;
        }
    }
    const bool roots_ok = kv_set(*st.db, "lib.roots", joined);
    return kv_set(*st.db, "lib.scan_on_start", st.scan_start ? "1" : "0") && roots_ok;
}

INT_PTR on_command(HWND dlg, DialogState& st, WPARAM wp)
{
    switch (LOWORD(wp)) {
    case IDC_ROOT_ADD:
        add_root(dlg, st);
        return TRUE;
    case IDC_ROOT_REMOVE:
        remove_root(dlg, st);
        return TRUE;
    case IDOK:
        st.scan_start = IsDlgButtonChecked(dlg, IDC_SCAN_ON_START) == BST_CHECKED;
        if (!save(st)) {
            st.changed = false;
            show_save_error(dlg);
        }
        EndDialog(dlg, 1);
        return TRUE;
    case IDC_UNHIDE:
        if (!unhide_all_books(*st.db))
            show_save_error(dlg);
        return TRUE;
    case IDCANCEL:
        st.changed = false;
        EndDialog(dlg, 0);
        return TRUE;
    default:
        return FALSE;
    }
}

INT_PTR CALLBACK dialog_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) noexcept
{
    try {
        if (msg == WM_INITDIALOG) {
            auto* st = reinterpret_cast<DialogState*>(lp); // NOLINT(performance-no-int-to-ptr): Win32 dialog ABI
            SetWindowLongPtrW(dlg, DWLP_USER, lp);
            fill_list(dlg, *st);
            CheckDlgButton(dlg, IDC_SCAN_ON_START, st->scan_start ? BST_CHECKED : BST_UNCHECKED);
            return TRUE;
        }
        auto* st = reinterpret_cast<DialogState*>(GetWindowLongPtrW(dlg, DWLP_USER)); // NOLINT(performance-no-int-to-ptr): Win32 dialog ABI
        if (msg == WM_COMMAND && st != nullptr)
            return on_command(dlg, *st, wp);
    } catch (...) {
        log::error("exception in library settings dialog");
    }
    return FALSE;
}

} // namespace

std::vector<std::filesystem::path> load_extra_roots(Db& db)
{
    std::vector<std::filesystem::path> out;
    const auto v = kv_get(db, "lib.roots");
    if (!v)
        return out;
    size_t pos = 0;
    while (pos < v->size() && out.size() < kMaxExtraRoots) {
        const size_t end = std::min(v->find('\n', pos), v->size());
        if (end > pos && end - pos <= kMaxRootChars) {
            if (auto w = utf8_to_wide(std::string_view(*v).substr(pos, end - pos)); w)
                out.emplace_back(std::move(*w));
        }
        pos = end + 1;
    }
    return out;
}

bool scan_on_start(Db& db) noexcept
{
    const auto v = kv_get(db, "lib.scan_on_start");
    return !v || *v != "0";
}

bool show_library_settings(HINSTANCE inst, HWND owner, Db& db) noexcept
{
    try {
        DialogState st{&db, {}, scan_on_start(db), false};
        for (const auto& p : load_extra_roots(db))
            st.roots.push_back(p.wstring());
        DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_LIBRARY), owner, &dialog_proc, reinterpret_cast<LPARAM>(&st));
        return st.changed;
    } catch (...) {
        log::error("library settings failed");
        return false;
    }
}

}
