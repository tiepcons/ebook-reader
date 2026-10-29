#include "TtsEngines.h"

#include "Kv.h"
#include "UiMessages.h"
#include "base/Log.h"
#include "base/SecretStore.h"

namespace eb {
namespace {

std::unique_ptr<PiperTts> make_piper(const AppPaths& paths)
{
    auto piper = std::make_unique<PiperTts>(paths.libs / L"piper", paths.voices, paths.tts_cache / L"piper" / L"tmp");
    return piper->voices().empty() ? nullptr : std::move(piper);
}

std::unique_ptr<VieNeuTts> make_vieneu(const AppPaths& paths)
{
    const std::filesystem::path libs = paths.libs / L"vieneu";
    const std::filesystem::path data = paths.data / L"vieneu";
    const std::filesystem::path temp = paths.tts_cache / L"vieneu" / L"tmp";
    if (!vieneu::find_assets(vieneu::make_cfg(libs, data, temp)))
        return nullptr;
    return std::make_unique<VieNeuTts>(libs, data, temp);
}

std::unique_ptr<AzureTts> make_azure(const AppPaths& paths, Db& db, HWND sink)
{
    const auto region = kv_get(db, kAzureRegionKey);
    if (!region || !secret_get(kAzureSecretTarget))
        return nullptr;
    AzureConfig cfg;
    cfg.region = *region;
    cfg.db_path = paths.data / L"library.db";
    cfg.cache_dir = paths.tts_cache / L"azure";
    cfg.notify = sink;
    auto azure = std::make_unique<AzureTts>(std::move(cfg));
    return azure->usable() ? std::move(azure) : nullptr;
}

}

void reload_azure_engine(TtsEngines& e, const AppPaths& paths, Db& db, HWND sink) noexcept
{
    e.azure.reset();
    try {
        e.azure = make_azure(paths, db, sink);
        if (e.azure)
            e.azure->set_sink(sink, WM_APP_TTS_EVENT);
    } catch (...) {
        e.azure.reset();
        log::error("tts: the Azure engine could not start");
    }
}

TtsEngines make_tts_engines(const AppPaths& paths, Db& db, HWND sink) noexcept
{
    TtsEngines e;
    try {
        e.winrt = std::make_unique<WinRtTts>();
        e.winrt->set_sink(sink, WM_APP_TTS_EVENT);
    } catch (...) {
        e.winrt.reset();
        log::error("tts: the Windows voices are not available");
    }
    try {
        e.piper = make_piper(paths);
        if (e.piper)
            e.piper->set_sink(sink, WM_APP_TTS_EVENT);
        e.azure = make_azure(paths, db, sink);
        if (e.azure)
            e.azure->set_sink(sink, WM_APP_TTS_EVENT);
        e.vieneu = make_vieneu(paths);
        if (e.vieneu)
            e.vieneu->set_sink(sink, WM_APP_TTS_EVENT);
    } catch (...) {
        log::error("tts: an optional voice engine could not start");
    }
    return e;
}

}
