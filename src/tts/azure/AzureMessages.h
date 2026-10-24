#pragma once
#include <windows.h>

namespace eb {

// Engine thread -> notify window; wParam = Warn level (P80, P95, P100) of the monthly Azure free quota.
inline constexpr UINT WM_APP_AZURE_WARN = WM_APP + 40;

}
