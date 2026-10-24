#pragma once
#include "base/SecretStore.h"
#include "tts/TtsEngine.h"
#include "tts/azure/AudioCache.h"
#include "tts/azure/AzureHttp.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>

namespace eb {

enum class AzureOnQuota : uint8_t { Fallback, Ask, Stop };

struct AzureConfig {
    std::string region;
    AzureOnQuota on_quota = AzureOnQuota::Fallback;
    std::filesystem::path db_path;   // library database; the engine opens its own connection for the usage counter
    std::filesystem::path cache_dir; // data\tts-cache\azure
    uint64_t cache_cap_bytes = kDefaultAudioCacheBytes;
    HWND notify = nullptr; // receives WM_APP_AZURE_WARN
    std::wstring picked_voice; // always listed by voices()
    std::chrono::milliseconds backoff_unit{500}; // retries wait 1x, 2x, 4x
    std::function<Result<SecretString>()> key_source; // default: the Credential Manager entry
    std::unique_ptr<HttpClient> http;                 // default: WinHTTP
};

// Azure Speech REST engine. Errors: kTtsAuth (no key, 401/403), kTtsNetwork (no answer, 429/5xx after retries),
// kTtsQuota (monthly counter at 100%), kTtsSynthFailed (other HTTP statuses), kTtsPlayFailed (undecodable audio).
// voices() blocks on the network the first time; call it off the UI thread.
class AzureTts final : public TtsEngine {
public:
    explicit AzureTts(AzureConfig cfg);
    ~AzureTts() override;
    AzureTts(const AzureTts&) = delete;
    AzureTts& operator=(const AzureTts&) = delete;

    // False when the region is invalid: every speak() then fails with kTtsAuth.
    [[nodiscard]] bool usable() const noexcept;

    std::vector<Voice> voices() noexcept override;
    bool speak(uint32_t utterance_id, std::string_view utf8, const std::wstring& voice_id, double rate,
        double volume) noexcept override;
    void prepare(uint32_t utterance_id, std::string_view utf8, const std::wstring& voice_id, double rate) noexcept override;
    void pause() noexcept override;
    void resume() noexcept override;
    void stop() noexcept override;
    void set_sink(HWND hwnd, UINT msg) noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
