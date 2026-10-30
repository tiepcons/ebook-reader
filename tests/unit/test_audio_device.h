#pragma once
#include <windows.h>

#include <mmdeviceapi.h>

#include <thread>

namespace ebtest {

// True when a default audio render endpoint exists (CI runners have none, so playback never finishes there).
// Probed once, on a private thread so the caller's COM apartment is untouched.
inline bool has_audio_output()
{
    static const bool present = [] {
        bool ok = false;
        std::thread probe([&ok] {
            if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
                return;
            IMMDeviceEnumerator* en = nullptr;
            if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) {
                IMMDevice* dev = nullptr;
                if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) {
                    ok = true;
                    dev->Release();
                }
                en->Release();
            }
            CoUninitialize();
        });
        probe.join();
        return ok;
    }();
    return present;
}

}
