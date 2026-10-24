#include "CloseAsk.h"

#include "Kv.h"

#include <commctrl.h>

#include <array>

namespace eb {
namespace {

constexpr const char* kKey = "ui.close_action";

}

CloseAction parse_close_action(std::string_view value) noexcept
{
    if (value == "exit")
        return CloseAction::Exit;
    if (value == "tray")
        return CloseAction::Tray;
    return CloseAction::Ask;
}

const char* close_action_name(CloseAction a) noexcept
{
    switch (a) {
    case CloseAction::Exit:
        return "exit";
    case CloseAction::Tray:
        return "tray";
    default:
        return "ask";
    }
}

CloseAnswer close_answer(int button, bool verified) noexcept
{
    if (button == kButtonExit)
        return {CloseChoice::Exit, verified};
    if (button == kButtonTray)
        return {CloseChoice::Tray, verified};
    return {CloseChoice::Cancel, false};
}

std::optional<CloseAction> remembered_action(const CloseAnswer& a) noexcept
{
    if (!a.remember)
        return std::nullopt;
    if (a.choice == CloseChoice::Exit)
        return CloseAction::Exit;
    if (a.choice == CloseChoice::Tray)
        return CloseAction::Tray;
    return std::nullopt;
}

CloseAction load_close_action(Db& db) noexcept
{
    const auto v = kv_get(db, kKey);
    return v ? parse_close_action(*v) : CloseAction::Ask;
}

bool store_close_action(Db& db, CloseAction a) noexcept
{
    return kv_set(db, kKey, close_action_name(a));
}

CloseAnswer ask_close(HWND owner) noexcept
{
    const std::array<TASKDIALOG_BUTTON, 3> buttons{{{kButtonExit, L"Thoát hẳn"}, {kButtonTray, L"Thu nhỏ xuống khay"}, {IDCANCEL, L"Hủy"}}};
    TASKDIALOGCONFIG cfg{};
    cfg.cbSize = sizeof(cfg);
    cfg.hwndParent = owner;
    cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    cfg.pszWindowTitle = L"Ebook Reader";
    cfg.pszMainInstruction = L"Bạn muốn làm gì khi đóng cửa sổ?";
    cfg.pButtons = buttons.data();
    cfg.cButtons = static_cast<UINT>(buttons.size());
    cfg.nDefaultButton = kButtonTray;
    cfg.pszVerificationText = L"Ghi nhớ lựa chọn";
    int button = IDCANCEL;
    BOOL verified = FALSE;
    if (FAILED(TaskDialogIndirect(&cfg, &button, nullptr, &verified)))
        return {};
    return close_answer(button, verified != FALSE);
}

}
