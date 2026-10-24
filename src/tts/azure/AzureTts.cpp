#include "tts/azure/AzureTts.h"

#include "base/Log.h"
#include "base/Text.h"
#include "lib/AzureUsage.h"
#include "lib/Db.h"
#include "tts/azure/AzureMessages.h"
#include "tts/azure/AzurePlayer.h"
#include "tts/azure/AzureProto.h"

#include <cstring>
#include <mutex>
#include <optional>

namespace eb {
namespace {

constexpr int kMaxRetries = 3;
constexpr std::wstring_view kHostSuffix = L".tts.speech.microsoft.com";
constexpr std::wstring_view kSynthPath = L"/cognitiveservices/v1";
constexpr std::wstring_view kVoicesPath = L"/cognitiveservices/voices/list";
constexpr std::wstring_view kSynthHeaders = L"Content-Type: application/ssml+xml\r\n"
                                            L"X-Microsoft-OutputFormat: audio-24khz-48kbitrate-mono-mp3\r\n"
                                            L"User-Agent: EbookReader";
constexpr std::wstring_view kVoicesHeaders = L"User-Agent: EbookReader";

struct Req {
    bool speak = false;
    uint32_t gen = 0;
    uint32_t epoch = 0;
    uint32_t id = 0;
    std::string text;
    std::wstring voice;
    double rate = 1.0;
    double volume = 0.0;
};

struct Fetched {
    uint8_t err = 0;
    AzureAudio audio;
};

int64_t now_unix() noexcept
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

uint8_t status_error(const HttpResponse& r) noexcept
{
    if (r.status == 200)
        return r.body.empty() ? kTtsSynthFailed : 0;
    if (r.status == 401 || r.status == 403)
        return kTtsAuth;
    if (r.status == 0 || r.status == 429 || r.status >= 500)
        return kTtsNetwork;
    return kTtsSynthFailed;
}

bool retryable(int status) noexcept
{
    return status == 429 || status >= 500;
}

}

struct AzureTts::Impl {
    AzureConfig cfg;
    bool ok;
    std::shared_ptr<AzureSink> sink = std::make_shared<AzureSink>();
    AudioCache cache;
    std::optional<Db> db;
    AzurePlayer player;
    std::mutex voices_mutex;
    std::vector<Voice> voice_list;
    AzureTaskThread net;

    explicit Impl(AzureConfig c)
        : cfg(std::move(c)), ok(valid_azure_region(cfg.region)), cache(cfg.cache_dir, cfg.cache_cap_bytes), player(sink)
    {
        if (!cfg.http)
            cfg.http = make_winhttp_client();
        ok = ok && cfg.http != nullptr;
        if (!cfg.key_source)
            cfg.key_source = [] { return secret_get(kAzureSecretTarget); };
        if (!cfg.db_path.empty()) {
            if (auto d = Db::open(cfg.db_path))
                db.emplace(std::move(*d));
            else
                log::error("azure: usage database unavailable");
        }
    }

    HttpRequest base_request(std::wstring_view path, bool post, std::wstring_view headers) const
    {
        HttpRequest rq;
        rq.post = post;
        rq.host = std::wstring(cfg.region.begin(), cfg.region.end()) + std::wstring(kHostSuffix);
        rq.path = std::wstring(path);
        rq.headers = std::wstring(headers);
        return rq;
    }

    void warn(Warn level) const noexcept
    {
        if (cfg.notify != nullptr)
            PostMessageW(cfg.notify, WM_APP_AZURE_WARN, static_cast<WPARAM>(level), 0);
    }

    uint8_t quota_gate(const Req& r)
    {
        if (!db)
            return 0;
        const auto st = AzureUsage(*db).status(now_unix());
        if (!st || !st->over_quota)
            return 0;
        if (cfg.on_quota == AzureOnQuota::Ask && r.speak)
            warn(Warn::P100);
        return kTtsQuota;
    }

    void record_usage(std::string_view text)
    {
        if (!db)
            return;
        AzureUsage usage(*db);
        const int64_t now = now_unix();
        if (!usage.add(now, billable_chars(text)))
            log::error("azure: usage counter update failed");
        if (const auto w = usage.check_warning(now); w && *w != Warn::None)
            warn(*w);
    }

    HttpResponse send_with_retry(const HttpRequest& rq, const Req& r)
    {
        HttpResponse resp;
        for (int attempt = 0; attempt <= kMaxRetries; ++attempt) {
            resp = cfg.http->send(rq);
            log::info("azure: synth http status {} attempt {}", resp.status, attempt);
            if (!retryable(resp.status) || attempt == kMaxRetries)
                break;
            if (!sink->sleep_while_valid(cfg.backoff_unit * (1 << attempt), r.speak, r.gen, r.epoch))
                return {};
        }
        return resp;
    }

    Fetched download(const Req& r, const std::string& voice, int pct, const std::string& cache_key)
    {
        const auto secret = cfg.key_source();
        if (!secret || !validate_api_key(*secret))
            return {kTtsAuth, nullptr};
        HttpRequest rq = base_request(kSynthPath, true, kSynthHeaders);
        rq.body = build_ssml(r.text, voice, pct);
        rq.key = &*secret;
        const HttpResponse resp = send_with_retry(rq, r);
        if (const uint8_t err = status_error(resp); err != 0)
            return {err, nullptr};
        record_usage(r.text);
        auto bytes = std::make_shared<std::vector<std::byte>>(resp.body.size());
        std::memcpy(bytes->data(), resp.body.data(), resp.body.size());
        cache.put(cache_key, *bytes);
        return {0, std::move(bytes)};
    }

    Fetched fetch(const Req& r)
    {
        std::string voice(kAzureDefaultVoice);
        if (!r.voice.empty()) {
            auto v = wide_to_utf8(r.voice);
            voice = v ? std::move(*v) : std::string();
        }
        if (!valid_azure_voice(voice))
            return {kTtsBadVoice, nullptr};
        const int pct = rate_percent(r.rate);
        const std::string key = AudioCache::key_for(voice, pct, r.text);
        if (key.empty())
            return {kTtsSynthFailed, nullptr};
        if (auto hit = cache.get(key))
            return {0, std::make_shared<const std::vector<std::byte>>(std::move(*hit))};
        if (!sink->valid(r.speak, r.gen, r.epoch))
            return {kTtsSynthFailed, nullptr};
        if (const uint8_t e = quota_gate(r); e != 0)
            return {e, nullptr};
        return download(r, voice, pct, key);
    }

    void run_speak(const Req& r)
    {
        if (!sink->current(r.gen))
            return;
        const Fetched f = fetch(r);
        if (!sink->current(r.gen))
            return;
        if (f.err != 0)
            sink->post(r.gen, r.id, TtsEvent::Error, f.err);
        else
            player.play(r.gen, r.id, f.audio, r.volume);
    }

    void run_prepare(const Req& r)
    {
        if (!sink->valid(false, 0, r.epoch))
            return;
        const Fetched f = fetch(r);
        if (f.err != 0)
            log::info("azure: prefetch failed with code {}", static_cast<int>(f.err));
    }

    std::vector<Voice> list_voices()
    {
        const std::scoped_lock lk(voices_mutex);
        if (!voice_list.empty() || !ok)
            return voice_list;
        const auto secret = cfg.key_source();
        if (!secret || !validate_api_key(*secret))
            return {};
        HttpRequest rq = base_request(kVoicesPath, false, kVoicesHeaders);
        rq.cancellable = false;
        rq.key = &*secret;
        const HttpResponse resp = cfg.http->send(rq);
        log::info("azure: voices http status {}", resp.status);
        if (resp.status == 200)
            voice_list = parse_voices(resp.body, cfg.picked_voice);
        return voice_list;
    }
};

AzureTts::AzureTts(AzureConfig cfg) : impl_(std::make_unique<Impl>(std::move(cfg))) {}

AzureTts::~AzureTts()
{
    impl_->sink->invalidate();
    if (impl_->cfg.http)
        impl_->cfg.http->cancel();
    impl_->sink->clear();
}

bool AzureTts::usable() const noexcept
{
    return impl_->ok;
}

std::vector<Voice> AzureTts::voices() noexcept
{
    try {
        return impl_->list_voices();
    } catch (...) {
        return {};
    }
}

bool AzureTts::speak(uint32_t id, std::string_view utf8, const std::wstring& voice_id, double rate, double volume) noexcept
{
    try {
        const uint32_t gen = impl_->sink->begin_speak();
        impl_->player.stop();
        if (!impl_->ok || utf8.empty() || utf8.size() > kTtsMaxUtf8) {
            const uint32_t err = !impl_->ok ? kTtsAuth : utf8.empty() ? kTtsSynthFailed : kTtsTextTooLarge;
            impl_->sink->post(gen, id, TtsEvent::Error, err);
            return false;
        }
        auto req = std::make_shared<const Req>(Req{true, gen, 0, id, std::string(utf8), voice_id, rate, volume});
        impl_->net.push([im = impl_.get(), req] { im->run_speak(*req); });
        return true;
    } catch (...) {
        return false;
    }
}

void AzureTts::prepare(uint32_t id, std::string_view utf8, const std::wstring& voice_id, double rate) noexcept
{
    try {
        if (!impl_->ok || utf8.empty() || utf8.size() > kTtsMaxUtf8)
            return;
        auto req = std::make_shared<const Req>(Req{false, 0, impl_->sink->epoch(), id, std::string(utf8), voice_id, rate, 0.0});
        impl_->net.push([im = impl_.get(), req] { im->run_prepare(*req); });
    } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
    }
}

void AzureTts::pause() noexcept
{
    impl_->player.pause();
}

void AzureTts::resume() noexcept
{
    impl_->player.resume();
}

void AzureTts::stop() noexcept
{
    impl_->sink->invalidate();
    if (impl_->cfg.http)
        impl_->cfg.http->cancel();
    impl_->player.stop();
}

void AzureTts::set_sink(HWND hwnd, UINT msg) noexcept
{
    impl_->sink->set(hwnd, msg);
}

}
