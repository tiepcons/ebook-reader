#pragma once
#include "FontPrefs.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <optional>

namespace eb {

using PrefsApply = std::function<void(const ReaderPrefs&)>;

// Modal. Every change is passed to `live` at once; returns the accepted settings, or nullopt after Cancel
// (the caller then restores `initial`).
std::optional<ReaderPrefs> show_reader_settings(HINSTANCE inst, HWND owner, const ReaderPrefs& initial,
    const PrefsApply& live) noexcept;

// Dark title bar when the page colour is dark; nullopt restores the light title bar. Menus and controls keep system colours.
void apply_reader_chrome(HWND main, std::optional<uint32_t> bg_rgb) noexcept;

}
