#pragma once
#include <windows.h>

#include <cstdlib>
#include <string>

namespace ebtest {

// Wall-clock budgets are strict locally; slow CI runners set EB_PERF_SCALE (1..20) to loosen them.
inline double perf_budget(double ms)
{
    static const double scale = [] {
        char buf[32];
        const DWORD n = GetEnvironmentVariableA("EB_PERF_SCALE", buf, sizeof(buf));
        if (n == 0 || n >= sizeof(buf))
            return 1.0;
        char* end = nullptr;
        const double v = std::strtod(buf, &end);
        return (end != buf && v >= 1.0 && v <= 20.0) ? v : 1.0;
    }();
    return ms * scale;
}

}
