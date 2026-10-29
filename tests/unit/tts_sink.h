#pragma once
#include "tts/TtsEngine.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

// Message-only window that collects the events of a TtsEngine for the engine tests.
namespace eb_test {

using Clock = std::chrono::steady_clock;
inline constexpr UINT kMsg = WM_APP + 9;

struct Got {
    uint32_t id;
    eb::TtsEvent ev;
    uint32_t value;
    Clock::time_point at;
};

inline std::vector<Got>* g_got = nullptr;
inline std::function<void(const Got&)> g_on_event;

inline LRESULT CALLBACK sink_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == kMsg) {
        if (g_got != nullptr) {
            g_got->push_back({static_cast<uint32_t>(w), eb::tts_event(l), eb::tts_value(l), Clock::now()});
            if (g_on_event)
                g_on_event(g_got->back());
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

struct Sink {
    HWND hwnd = nullptr;
    std::vector<Got> got;

    Sink()
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = sink_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"EbEngineTestSink";
        RegisterClassW(&wc);
        hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        g_got = &got;
    }
    ~Sink()
    {
        g_got = nullptr;
        g_on_event = nullptr;
        DestroyWindow(hwnd);
    }
    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;

    static void pump_pending()
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            DispatchMessageW(&msg);
    }
    void pump_ms(int ms)
    {
        const auto end = Clock::now() + std::chrono::milliseconds(ms);
        while (Clock::now() < end) {
            pump_pending();
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        }
    }
    bool wait_for(uint32_t id, eb::TtsEvent ev, int timeout_ms)
    {
        const auto end = Clock::now() + std::chrono::milliseconds(timeout_ms);
        while (Clock::now() < end) {
            pump_pending();
            for (const auto& g : got)
                if (g.id == id && (g.ev == ev || g.ev == eb::TtsEvent::Error))
                    return g.ev == ev;
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        }
        return false;
    }
};

}
