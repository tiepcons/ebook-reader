#pragma once
#include <windows.h>

namespace eb {

inline constexpr UINT kMsgRescan = WM_APP + 1;
inline constexpr UINT WM_APP_SCAN_PROGRESS = WM_APP + 2; // wParam = files done, lParam = total
inline constexpr UINT WM_APP_SCAN_DONE = WM_APP + 3;     // wParam = 1 when the scan finished without error
inline constexpr UINT WM_APP_OPEN_BOOK = WM_APP + 4;     // the id is taken from MainWnd::take_open_request

inline constexpr UINT_PTR kSearchTimer = 1;
inline constexpr UINT kSearchDebounceMs = 150;

}
