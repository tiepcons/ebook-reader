#include "GotoPercent.h"

#include "ReaderLogic.h"
#include "base/Log.h"
#include "resource.h"

#include <array>
#include <format>
#include <string>

namespace eb {
namespace {

constexpr int kMaxChars = 8;

struct State {
    int value = 0;
    bool accepted = false;
};

bool accept(HWND dlg, State& st)
{
    std::array<wchar_t, kMaxChars> buf{};
    GetDlgItemTextW(dlg, IDC_GOTO_PERCENT, buf.data(), static_cast<int>(buf.size()));
    const auto v = parse_percent(buf.data());
    if (!v) {
        MessageBeep(MB_ICONWARNING);
        SetFocus(GetDlgItem(dlg, IDC_GOTO_PERCENT));
        return false;
    }
    st.value = *v;
    st.accepted = true;
    return true;
}

INT_PTR CALLBACK dialog_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) noexcept
{
    try {
        if (msg == WM_INITDIALOG) {
            const auto* st = reinterpret_cast<State*>(lp); // NOLINT(performance-no-int-to-ptr): Win32 dialog ABI
            SetWindowLongPtrW(dlg, DWLP_USER, lp);
            SetDlgItemTextW(dlg, IDC_GOTO_PERCENT, std::format(L"{}", st->value).c_str());
            SendDlgItemMessageW(dlg, IDC_GOTO_PERCENT, EM_SETSEL, 0, -1);
            return TRUE;
        }
        auto* st = reinterpret_cast<State*>(GetWindowLongPtrW(dlg, DWLP_USER)); // NOLINT(performance-no-int-to-ptr): Win32 dialog ABI
        if (msg != WM_COMMAND || st == nullptr)
            return FALSE;
        if (LOWORD(wp) == IDOK) {
            if (accept(dlg, *st))
                EndDialog(dlg, IDOK);
        } else if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, IDCANCEL);
        } else {
            return FALSE;
        }
        return TRUE;
    } catch (...) {
        log::error("exception in go-to dialog");
        return FALSE;
    }
}

}

std::optional<int> ask_percent(HINSTANCE inst, HWND owner, int current) noexcept
{
    try {
        State st;
        st.value = current;
        DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_GOTO_PERCENT), owner, &dialog_proc, reinterpret_cast<LPARAM>(&st));
        return st.accepted ? std::optional<int>(st.value) : std::nullopt;
    } catch (...) {
        log::error("go-to dialog failed");
        return std::nullopt;
    }
}

}
