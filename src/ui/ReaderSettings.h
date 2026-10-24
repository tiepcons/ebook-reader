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

// Title bar (dark when the page colour is dark) and status strip in the reader's colours; nullopt restores the system look.
void apply_reader_chrome(HWND main, HWND status, std::optional<uint32_t> bg_rgb) noexcept;

}
