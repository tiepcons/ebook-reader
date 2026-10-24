#pragma once
#include "lib/Db.h"

#include <windows.h>

#include <optional>
#include <string_view>

namespace eb {

enum class CloseAction : unsigned char { Ask, Exit, Tray };
enum class CloseChoice : unsigned char { Exit, Tray, Cancel };

struct CloseAnswer {
    CloseChoice choice = CloseChoice::Cancel;
    bool remember = false;
};

inline constexpr int kButtonExit = 1001;
inline constexpr int kButtonTray = 1002;

// Anything but "exit" or "tray" means ask.
CloseAction parse_close_action(std::string_view value) noexcept;
const char* close_action_name(CloseAction a) noexcept;
CloseAnswer close_answer(int button, bool verified) noexcept;
// The preference to store for an answer; nothing when the user cancelled or left the box clear.
std::optional<CloseAction> remembered_action(const CloseAnswer& a) noexcept;

CloseAction load_close_action(Db& db) noexcept;
bool store_close_action(Db& db, CloseAction a) noexcept;

// Modal task dialog [Thoát hẳn] [Thu nhỏ xuống khay] [Hủy] with a "remember" checkbox. Cancel on any failure.
CloseAnswer ask_close(HWND owner) noexcept;

}
