#pragma once
#include <windows.h>

#include <objbase.h>

namespace eb {

// Initialises COM on the calling thread for its lifetime. model: COINIT_APARTMENTTHREADED or COINIT_MULTITHREADED.
class ComScope {
public:
    explicit ComScope(DWORD model) noexcept : ok_(SUCCEEDED(CoInitializeEx(nullptr, model))) {}
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    ~ComScope()
    {
        if (ok_)
            CoUninitialize();
    }
    [[nodiscard]] bool ok() const noexcept { return ok_; }

private:
    bool ok_;
};

}
