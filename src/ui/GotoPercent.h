#pragma once
#include <windows.h>

#include <optional>

namespace eb {

// Small modal box asking for a reading percentage 0..100; nullopt when cancelled.
std::optional<int> ask_percent(HINSTANCE inst, HWND owner, int current) noexcept;

}
