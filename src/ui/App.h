#pragma once
#include <windows.h>

namespace eb {

int run_app(HINSTANCE inst, const wchar_t* cmdline, int show) noexcept;

}
