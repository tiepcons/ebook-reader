#include "tts/azure/AzureTts.h"

#include "base/Log.h"
#include "lib/AzureUsage.h"
#include "lib/Schema.h"
#include "test_base_tmp.h"
#include "tts/azure/AzureMessages.h"
#include "tts/azure/AzureProto.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <deque>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;
constexpr UINT kMsg = WM_APP + 9;
constexpr const char* kBody = "\xFF\xF3\x40\xC0 not really an mp3";
constexpr std::wstring_view kKey = L"SECRETKEY0123456789abcdefghijklmn";
constexpr const char* kKeyAscii = "SECRETKEY0123456789abcdefghijklmn";
constexpr const char* kVoicesJson =
    R"([{"LocalName":"Hoài My","ShortName":"vi-VN-HoaiMyNeural","Locale":"vi-VN"},)"
    R"({"LocalName":"Denise","ShortName":"fr-FR-DeniseNeural","Locale":"fr-FR"}])";

struct Got {
    uint32_t id;
    eb::TtsEvent ev;
    uint32_t value;
};

std::vector<Got>* g_got = nullptr;
std::vector<WPARAM>* g_warns = nullptr;

LRESULT CALLBACK sink_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == kMsg && g_got != nullptr) {
        g_got->push_back({static_cast<uint32_t>(w), eb::tts_event(l), eb::tts_value(l)});
        return 0;
    }
    if (m == eb::WM_APP_AZURE_WARN && g_warns != nullptr) {
        g_warns->push_back(w);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

struct Seen {
    bool post;
    bool cancellable;
    std::wstring host, path, headers, key;
    std::string body;
};

class FakeHttp final : public eb::HttpClient {
public:
    eb::HttpResponse send(const eb::HttpRequest& r) noexcept override
    {
        std::unique_lock lk(m);
        const eb::HttpResponse peek = script.empty() ? eb::HttpResponse{200, kBody} : script.front();
        if (r.on_status && !hold)
            r.on_status(peek.status);
        seen.push_back({r.post, r.cancellable, r.host, r.path, r.headers, r.key ? std::wstring(r.key->view()) : L"", r.body});
        eb::HttpResponse out = script.empty() ? eb::HttpResponse{200, kBody} : script.front();
        if (!script.empty())
            script.pop_front();
        if (hold) {
            in_flight = true;
            lk.unlock();
            while (!cancelled)
                Sleep(2);
            return {};
        }
        return out;
    }
    void cancel() noexcept override
    {
        ++cancels;
        cancelled = true;
    }
    void shutdown() noexcept override
    {
        cancelled = true;
    }
    size_t calls()
    {
        const std::scoped_lock lk(m);
        return seen.size();
    }
    Seen first() { return at(0); }
    Seen at(size_t i)
    {
        const std::scoped_lock lk(m);
        return seen.at(i);
    }
    void set_script(std::deque<eb::HttpResponse> s)
    {
        const std::scoped_lock lk(m);
        script = std::move(s);
    }

    std::mutex m;
    std::deque<eb::HttpResponse> script;
    std::vector<Seen> seen;
    std::atomic<int> cancels{0};
    std::atomic<bool> cancelled{false};
    std::atomic<bool> in_flight{false};
    bool hold = false;
};

template <class P>
bool wait_until(P pred, int ms = 8000)
{
    const auto end = Clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (pred())
            return true;
        if (Clock::now() > end)
            return false;
        Sleep(2);
    }
}

void pump_for(int ms)
{
    wait_until([] { return false; }, ms);
}

std::string env_value(const char* name)
{
    char buf[64];
    const DWORD n = GetEnvironmentVariableA(name, buf, sizeof buf);
    return n > 0 && n < sizeof buf ? std::string(buf, n) : std::string();
}

uint64_t billed(std::string_view text, int pct = 0)
{
    return eb::billable_chars(eb::ssml_voice_body(text, pct));
}

int64_t now_s()
{
    return static_cast<int64_t>(std::time(nullptr));
}

struct Harness {
    ebtest::TempDir t;
    HWND hwnd = nullptr;
    std::vector<Got> got;
    std::vector<WPARAM> warns;
    FakeHttp* http = nullptr;
    std::optional<eb::Db> db;
    std::unique_ptr<eb::AzureTts> tts;

    explicit Harness(eb::AzureOnQuota quota = eb::AzureOnQuota::Fallback, std::string region = "southeastasia",
        bool with_db = true)
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = sink_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"EbAzureTestSink";
        RegisterClassW(&wc);
        hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        g_got = &got;
        g_warns = &warns;
        auto d = eb::Db::open(t.path() / L"lib.db");
        REQUIRE(d.has_value());
        REQUIRE(eb::migrate(*d).has_value());
        db.emplace(std::move(*d));

        eb::AzureConfig cfg;
        cfg.region = std::move(region);
        cfg.on_quota = quota;
        if (with_db)
            cfg.db_path = t.path() / L"lib.db";
        cfg.cache_dir = t.path() / L"cache";
        cfg.notify = hwnd;
        cfg.backoff_unit = std::chrono::milliseconds(1);
        cfg.voices_cooldown = std::chrono::milliseconds(300);
        cfg.key_source = [] { return eb::SecretString::copy_of(kKey); };
        auto fake = std::make_unique<FakeHttp>();
        http = fake.get();
        cfg.http = std::move(fake);
        tts = std::make_unique<eb::AzureTts>(std::move(cfg));
        tts->set_sink(hwnd, kMsg);
    }
    ~Harness()
    {
        tts.reset();
        g_got = nullptr;
        g_warns = nullptr;
        DestroyWindow(hwnd);
    }
    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;

    uint64_t used() { return eb::AzureUsage(*db).status(now_s())->used; }
    bool got_error(uint32_t id, uint32_t code)
    {
        for (const Got& g : got)
            if (g.id == id && g.ev == eb::TtsEvent::Error && g.value == code)
                return true;
        return false;
    }
    bool wait_used(uint64_t n) { return wait_until([&] { return used() == n; }); }
    bool wait_error(uint32_t id, uint32_t code) { return wait_until([&] { return got_error(id, code); }); }
};

}

TEST_CASE("tts azure retries 429 with backoff and counts usage once on 200")
{
    Harness h;
    h.http->script = {{429, ""}, {429, ""}, {200, kBody}};
    REQUIRE(h.tts->speak(1, "xin ch\xC3\xA0o", L"", 1.0, 1.0));
    REQUIRE(h.wait_used(billed("xin ch\xC3\xA0o")));
    CHECK(h.http->calls() == 3);
    const Seen s = h.http->first();
    CHECK(s.post);
    CHECK(s.host == L"southeastasia.tts.speech.microsoft.com");
    CHECK(s.path == L"/cognitiveservices/v1");
    CHECK(s.headers.find(L"application/ssml+xml") != std::wstring::npos);
    CHECK(s.headers.find(L"audio-24khz-48kbitrate-mono-mp3") != std::wstring::npos);
    CHECK(s.key == kKey);
    CHECK(s.body.find("<voice name=\"vi-VN-HoaiMyNeural\"><prosody rate=\"+0%\">xin ch\xC3\xA0o</prosody>") != std::string::npos);
    CHECK_FALSE(h.got_error(1, eb::kTtsNetwork));
}

TEST_CASE("tts azure auth failure is final and not billed")
{
    Harness h;
    h.http->script = {{401, ""}};
    REQUIRE(h.tts->speak(5, "hello", L"", 1.0, 1.0));
    REQUIRE(h.wait_error(5, eb::kTtsAuth));
    CHECK(h.http->calls() == 1);
    CHECK(h.used() == 0);

    h.http->script = {{403, ""}};
    REQUIRE(h.tts->speak(6, "hello again", L"", 1.0, 1.0));
    REQUIRE(h.wait_error(6, eb::kTtsAuth));
    CHECK(h.http->calls() == 2);
}

TEST_CASE("tts azure gives up with a network error after three retries")
{
    Harness h;
    h.http->script = {{503, ""}, {500, ""}, {429, ""}, {502, ""}};
    REQUIRE(h.tts->speak(7, "hello", L"", 1.0, 1.0));
    REQUIRE(h.wait_error(7, eb::kTtsNetwork));
    CHECK(h.http->calls() == 4);
    CHECK(h.used() == 0);
}

TEST_CASE("tts azure connection failure and other statuses map to distinct errors")
{
    Harness h;
    h.http->script = {{0, ""}, {400, ""}, {200, ""}};
    REQUIRE(h.tts->speak(1, "one", L"", 1.0, 1.0));
    REQUIRE(h.wait_error(1, eb::kTtsNetwork));
    REQUIRE(h.tts->speak(2, "two", L"", 1.0, 1.0));
    REQUIRE(h.wait_error(2, eb::kTtsSynthFailed));
    REQUIRE(h.tts->speak(3, "three", L"", 1.0, 1.0));
    REQUIRE(h.wait_error(3, eb::kTtsSynthFailed));
    CHECK(h.http->calls() == 3);
    CHECK(h.used() == billed("three"));
}

TEST_CASE("tts azure cache hit skips the network and the usage counter")
{
    Harness h;
    REQUIRE(h.tts->speak(1, "same text", L"", 1.0, 1.0));
    REQUIRE(h.wait_used(billed("same text")));
    REQUIRE(h.tts->speak(2, "same text", L"", 1.0, 1.0));
    h.tts->prepare(3, "other text", L"", 1.0);
    REQUIRE(wait_until([&] { return h.http->calls() == 2; }));
    REQUIRE(h.wait_used(billed("same text") + billed("other text")));
    CHECK(h.http->calls() == 2);

    h.tts->prepare(4, "other text", L"", 1.0);
    h.tts->prepare(5, "sentinel", L"", 1.0);
    REQUIRE(h.wait_used(billed("same text") + billed("other text") + billed("sentinel")));
    CHECK(h.http->calls() == 3);
}

TEST_CASE("tts azure prepare is silent on failure and fills the cache for speak")
{
    Harness h;
    h.http->script = {{401, ""}};
    h.tts->prepare(1, "prefetch me", L"", 1.0);
    REQUIRE(wait_until([&] { return h.http->calls() == 1; }));
    pump_for(50);
    CHECK(h.got.empty());

    h.http->set_script({{200, kBody}});
    h.tts->prepare(2, "prefetch me", L"", 1.5);
    REQUIRE(h.wait_used(billed("prefetch me", 50)));
    h.tts->speak(2, "prefetch me", L"", 1.5, 1.0);
    h.tts->prepare(3, "sentinel", L"", 1.0);
    REQUIRE(wait_until([&] { return h.http->calls() == 3; }));
    CHECK(h.http->at(1).body.find("rate=\"+50%\"") != std::string::npos);
}

TEST_CASE("tts azure quota exhausted blocks the request in every mode")
{
    for (const auto mode : {eb::AzureOnQuota::Fallback, eb::AzureOnQuota::Stop, eb::AzureOnQuota::Ask}) {
        Harness h(mode);
        REQUIRE(eb::AzureUsage(*h.db).calibrate(now_s(), eb::kDefaultAzureQuota).has_value());
        REQUIRE(h.tts->speak(1, "over quota", L"", 1.0, 1.0));
        REQUIRE(h.wait_error(1, eb::kTtsQuota));
        CHECK(h.http->calls() == 0);
        if (mode == eb::AzureOnQuota::Ask) {
            REQUIRE(wait_until([&] { return !h.warns.empty(); }));
            CHECK(h.warns.front() == static_cast<WPARAM>(eb::Warn::P100));
        } else {
            pump_for(30);
            CHECK(h.warns.empty());
        }
    }
}

TEST_CASE("tts azure posts a warning when usage crosses 80 percent")
{
    Harness h;
    REQUIRE(eb::AzureUsage(*h.db).calibrate(now_s(), 399995).has_value());
    REQUIRE(h.tts->speak(1, "a sentence of some length", L"", 1.0, 1.0));
    REQUIRE(wait_until([&] { return !h.warns.empty(); }));
    CHECK(h.warns.front() == static_cast<WPARAM>(eb::Warn::P80));
    CHECK(h.http->calls() == 1);
}

TEST_CASE("tts azure rejects bad region, voice and text without calling the API")
{
    {
        Harness h(eb::AzureOnQuota::Fallback, "evil.example.com/x");
        CHECK_FALSE(h.tts->usable());
        CHECK_FALSE(h.tts->speak(1, "hello", L"", 1.0, 1.0));
        REQUIRE(h.wait_error(1, eb::kTtsAuth));
        CHECK(h.http->calls() == 0);
    }
    Harness h;
    CHECK(h.tts->usable());
    REQUIRE(h.tts->speak(2, "hello", L"vi-VN-\"><evil", 1.0, 1.0));
    REQUIRE(h.wait_error(2, eb::kTtsBadVoice));
    CHECK_FALSE(h.tts->speak(3, "", L"", 1.0, 1.0));
    REQUIRE(h.wait_error(3, eb::kTtsSynthFailed));
    CHECK_FALSE(h.tts->speak(4, std::string(eb::kTtsMaxUtf8 + 1, 'a'), L"", 1.0, 1.0));
    REQUIRE(h.wait_error(4, eb::kTtsTextTooLarge));
    CHECK(h.http->calls() == 0);
}

TEST_CASE("tts azure stop cancels the request in flight and silences its events")
{
    Harness h;
    h.http->hold = true;
    REQUIRE(h.tts->speak(1, "slow", L"", 1.0, 1.0));
    REQUIRE(wait_until([&] { return h.http->in_flight.load(); }));
    h.tts->stop();
    REQUIRE(wait_until([&] { return h.http->cancels.load() >= 1; }));
    pump_for(100);
    CHECK(h.got.empty());
    CHECK(h.used() == 0);
}

TEST_CASE("tts azure voices are fetched in the background, cached for the session and fetched with GET")
{
    Harness h;
    h.http->script = {{200, kVoicesJson}};
    CHECK(h.tts->voices().empty());
    std::vector<eb::Voice> v;
    REQUIRE(wait_until([&] { return !(v = h.tts->voices()).empty(); }));
    REQUIRE(v.size() == 1);
    CHECK(v[0].id == L"vi-VN-HoaiMyNeural");
    CHECK(v[0].name == L"Hoài My");
    CHECK(v[0].lang == L"vi-VN");
    CHECK(h.tts->voices().size() == 1);
    REQUIRE(h.http->calls() == 1);
    const Seen s = h.http->first();
    CHECK_FALSE(s.post);
    CHECK_FALSE(s.cancellable);
    CHECK(s.path == L"/cognitiveservices/voices/list");
    CHECK(s.key == kKey);
}

TEST_CASE("tts azure failed voice fetches back off for the cooldown")
{
    Harness h;
    h.http->script = {{401, ""}, {200, "garbage"}, {200, kVoicesJson}};
    CHECK(h.tts->voices().empty());
    REQUIRE(wait_until([&] { return h.http->calls() == 1; }));
    pump_for(20);
    CHECK(h.tts->voices().empty());
    CHECK(h.http->calls() == 1);
    pump_for(350);
    CHECK(h.tts->voices().empty());
    REQUIRE(wait_until([&] { return h.http->calls() == 2; }));
    pump_for(350);
    CHECK(h.tts->voices().empty());
    REQUIRE(wait_until([&] { return !h.tts->voices().empty(); }));
    CHECK(h.http->calls() == 3);
}

TEST_CASE("tts azure destruction does not wait for a hung voice fetch")
{
    Harness h;
    h.http->hold = true;
    CHECK(h.tts->voices().empty());
    REQUIRE(wait_until([&] { return h.http->in_flight.load(); }));
}

TEST_CASE("tts azure quota gate fails closed without a usage database")
{
    Harness h(eb::AzureOnQuota::Fallback, "southeastasia", false);
    REQUIRE(h.tts->speak(1, "no counter", L"", 1.0, 1.0));
    REQUIRE(h.wait_error(1, eb::kTtsQuota));
    CHECK(h.http->calls() == 0);
}

TEST_CASE("tts azure bills the markup Azure counts as soon as the status is 200")
{
    Harness h;
    h.http->script = {{200, ""}};
    REQUIRE(h.tts->speak(1, "a<b", L"", 2.0, 1.0));
    REQUIRE(h.wait_error(1, eb::kTtsSynthFailed));
    CHECK(h.used() == billed("a<b", 100));
    CHECK(h.used() > 3);
}

TEST_CASE("tts azure honours Retry-After on 429 within the cap")
{
    Harness h;
    h.http->script = {{429, "", 1}, {200, kBody}};
    const auto t0 = Clock::now();
    REQUIRE(h.tts->speak(1, "slow down", L"", 1.0, 1.0));
    REQUIRE(h.wait_used(billed("slow down")));
    CHECK(Clock::now() - t0 >= std::chrono::milliseconds(900));
    CHECK(h.http->calls() == 2);
}

TEST_CASE("tts azure never writes the key into the log")
{
    const ebtest::TempDir lt;
    const auto log_path = lt.path() / L"app.log";
    eb::log::init(log_path);
    {
        Harness h;
        h.http->script = {{429, ""}, {200, kBody}, {401, ""}, {200, kVoicesJson}};
        REQUIRE(h.tts->speak(1, "log check one", L"", 1.0, 1.0));
        REQUIRE(h.wait_used(billed("log check one")));
        REQUIRE(h.tts->speak(2, "log check two", L"", 1.0, 1.0));
        REQUIRE(h.wait_error(2, eb::kTtsAuth));
        REQUIRE(wait_until([&] { return h.tts->voices().size() == 1; }));
    }
    eb::log::shutdown();
    const std::ifstream f(log_path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    CHECK(text.find("azure:") != std::string::npos);
    CHECK(text.find(kKeyAscii) == std::string::npos);
    CHECK(text.find("SECRETKEY") == std::string::npos);
    CHECK(text.find("log check") == std::string::npos);
}

TEST_CASE("tts azure live service lists voices with the stored key" * doctest::skip(env_value("EB_AZURE_LIVE") != "1"))
{
    const ebtest::TempDir t;
    eb::AzureConfig cfg;
    cfg.region = env_value("EB_AZURE_REGION");
    cfg.cache_dir = t.path() / L"cache";
    eb::AzureTts tts(std::move(cfg));
    REQUIRE(tts.usable());
    CHECK(wait_until([&] { return !tts.voices().empty(); }, 20000));
}

TEST_CASE("tts azure winhttp client refuses requests once shut down")
{
    auto client = eb::make_winhttp_client();
    REQUIRE(client != nullptr);
    client->shutdown();
    eb::HttpRequest rq;
    rq.host = L"example.com";
    rq.path = L"/";
    const auto t0 = Clock::now();
    CHECK(client->send(rq).status == 0);
    CHECK(Clock::now() - t0 < std::chrono::seconds(2));
}

TEST_CASE("tts azure winhttp shutdown aborts a request that is waiting on the network")
{
    auto client = eb::make_winhttp_client();
    REQUIRE(client != nullptr);
    eb::HttpRequest rq;
    rq.host = L"10.255.255.1";
    rq.path = L"/";
    const auto t0 = Clock::now();
    std::atomic<int> status{-1};
    std::thread worker([&] { status = client->send(rq).status; });
    Sleep(200);
    client->shutdown();
    worker.join();
    CHECK(status.load() == 0);
    CHECK(Clock::now() - t0 < std::chrono::seconds(3));
}
