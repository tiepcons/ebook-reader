#pragma once
#include "AzureSettings.h"
#include "TtsPrefs.h"
#include "tts/TtsEngine.h"

#include <windows.h>

#include <optional>
#include <vector>

namespace eb {

// What the voice dialog asks of the application.
class TtsDialogApi {
public:
    virtual ~TtsDialogApi() = default;
    // Engines that can be used on this machine; the others are not offered.
    virtual std::vector<TtsKind> available() = 0;
    // Never blocks: the Azure list may be empty at first and fill in later.
    virtual std::vector<Voice> voices(TtsKind k) = 0;
    virtual void preview(TtsKind k, const std::wstring& voice, Lang lang, double rate) = 0;
    virtual void end_preview() = 0;
    virtual AzureView azure_view() = 0;
    virtual bool azure_save(const AzureEdits& e) = 0;
    virtual bool azure_reset_used() = 0;
};

// Modal. Returns the accepted settings, or nullopt after Cancel.
std::optional<TtsPrefs> show_tts_settings(HINSTANCE inst, HWND owner, const TtsPrefs& initial, TtsDialogApi& api) noexcept;

}
