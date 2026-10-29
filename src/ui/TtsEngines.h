#pragma once
#include "TtsController.h"
#include "base/Paths.h"
#include "lib/Db.h"
#include "tts/azure/AzureTts.h"
#include "tts/piper/PiperTts.h"
#include "tts/vieneu/VieNeuTts.h"
#include "tts/winrt/WinRtTts.h"

#include <memory>

namespace eb {

inline constexpr const char* kAzureRegionKey = "tts.azure.region";

// The engines this machine can use. WinRT is always tried; Piper needs libs\piper\piper.exe and at least one voice;
// Azure needs a key in the Credential Manager and a region;
// VieNeu needs libsieneu, dataieneu (model, sea_g2p.bin) and at least one voice preset.
struct TtsEngines {
    std::unique_ptr<WinRtTts> winrt;
    std::unique_ptr<PiperTts> piper;
    std::unique_ptr<AzureTts> azure;
    std::unique_ptr<VieNeuTts> vieneu;

    [[nodiscard]] TtsController::Engines pointers() const noexcept
    {
        return {winrt.get(), piper.get(), azure.get(), vieneu.get()};
    }
};

// Engine events go to `sink` as WM_APP_TTS_EVENT; Azure quota warnings go to `sink` too.
[[nodiscard]] TtsEngines make_tts_engines(const AppPaths& paths, Db& db, HWND sink) noexcept;
// Replaces only the Azure engine (after its key or region changed). No controller may hold the old one.
void reload_azure_engine(TtsEngines& e, const AppPaths& paths, Db& db, HWND sink) noexcept;

}
