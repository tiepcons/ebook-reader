#include "tts/WinRtTts.h"

#include <doctest/doctest.h>

#include <chrono>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
constexpr UINT kMsg = WM_APP + 7;

struct Got {
    uint32_t id;
    eb::TtsEvent ev;
    uint32_t value;
    Clock::time_point at;
};

std::vector<Got>* g_got = nullptr;

LRESULT CALLBACK sink_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == kMsg) {
        if (g_got != nullptr)
            g_got->push_back({static_cast<uint32_t>(w), eb::tts_event(l), eb::tts_value(l), Clock::now()});
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
        wc.lpszClassName = L"EbTtsTestSink";
        RegisterClassW(&wc);
        hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        g_got = &got;
    }
    ~Sink()
    {
        g_got = nullptr;
        DestroyWindow(hwnd);
    }
    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;

    void pump_ms(int ms)
    {
        const auto end = Clock::now() + std::chrono::milliseconds(ms);
        while (Clock::now() < end) {
            pump_pending();
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        }
    }
    static void pump_pending()
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            DispatchMessageW(&msg);
    }
    bool wait_for(eb::TtsEvent ev, int timeout_ms)
    {
        const auto end = Clock::now() + std::chrono::milliseconds(timeout_ms);
        while (Clock::now() < end) {
            pump_pending();
            for (const auto& g : got)
                if (g.ev == ev || g.ev == eb::TtsEvent::Error)
                    return g.ev == ev;
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        }
        return false;
    }
};

std::wstring find_voice(eb::WinRtTts& tts, std::wstring_view lang_prefix)
{
    for (const auto& v : tts.voices())
        if (v.lang.starts_with(lang_prefix))
            return v.id;
    return {};
}

void check_words(const std::vector<Got>& got, std::string_view text)
{
    REQUIRE(!got.empty());
    CHECK(got.front().ev == eb::TtsEvent::Started);
    CHECK(got.back().ev == eb::TtsEvent::Finished);
    int words = 0;
    int64_t prev = -1;
    for (const auto& g : got) {
        if (g.ev != eb::TtsEvent::Word)
            continue;
        ++words;
        REQUIRE(g.value < text.size());
        CHECK(static_cast<int64_t>(g.value) > prev);
        prev = g.value;
        CHECK((g.value == 0 || text[g.value - 1] == ' '));
        CHECK(text[g.value] != ' ');
    }
    CHECK(words >= 3);
}

void read_sentence(std::wstring_view lang, std::string_view text)
{
    Sink sink;
    eb::WinRtTts tts;
    tts.set_sink(sink.hwnd, kMsg);
    const auto voice = find_voice(tts, lang);
    if (voice.empty()) {
        MESSAGE("no voice for requested language, skipped");
        return;
    }
    REQUIRE(tts.speak(1, text, voice, 1.0, 0.0));
    REQUIRE(sink.wait_for(eb::TtsEvent::Finished, 20000));
    for (const auto& g : sink.got)
        CHECK(g.id == 1);
    check_words(sink.got, text);
}

}

TEST_CASE("tts voices lists vi-VN and en-US")
{
    eb::WinRtTts tts;
    const auto v = tts.voices();
    if (find_voice(tts, L"vi").empty() || find_voice(tts, L"en-US").empty()) {
        MESSAGE("vi-VN or en-US voice missing, skipped");
        return;
    }
    CHECK(!v.empty());
}

TEST_CASE("tts winrt Vietnamese word boundaries")
{
    read_sentence(L"vi", "Trăm năm trong cõi người ta.");
}

TEST_CASE("tts winrt English word boundaries")
{
    read_sentence(L"en-US", "The quick brown fox jumps over the lazy dog.");
}

TEST_CASE("tts winrt unknown voice posts Error")
{
    Sink sink;
    eb::WinRtTts tts;
    tts.set_sink(sink.hwnd, kMsg);
    REQUIRE(tts.speak(5, "hello", L"no-such-voice", 1.0, 0.0));
    CHECK(!sink.wait_for(eb::TtsEvent::Finished, 10000));
    REQUIRE(!sink.got.empty());
    CHECK(sink.got.back().ev == eb::TtsEvent::Error);
    CHECK(sink.got.back().value == eb::kTtsBadVoice);
}

TEST_CASE("tts winrt prepared utterance starts without a gap")
{
    Sink sink;
    eb::WinRtTts tts;
    tts.set_sink(sink.hwnd, kMsg);
    const auto voice = find_voice(tts, L"en-US");
    if (voice.empty())
        return;
    REQUIRE(tts.speak(1, "Hello there.", voice, 1.0, 0.0));
    tts.prepare(2, "General Kenobi.", voice, 1.0);
    REQUIRE(sink.wait_for(eb::TtsEvent::Finished, 20000));
    sink.pump_ms(1500);
    sink.got.clear();
    const auto t0 = Clock::now();
    REQUIRE(tts.speak(2, "General Kenobi.", voice, 1.0, 0.0));
    REQUIRE(sink.wait_for(eb::TtsEvent::Started, 5000));
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(sink.got.front().at - t0).count() < 300);
    REQUIRE(sink.wait_for(eb::TtsEvent::Finished, 20000));
}

TEST_CASE("tts winrt stop silences further events")
{
    Sink sink;
    eb::WinRtTts tts;
    tts.set_sink(sink.hwnd, kMsg);
    const auto voice = find_voice(tts, L"en-US");
    if (voice.empty())
        return;
    REQUIRE(tts.speak(1, "One two three four five six seven eight nine ten eleven twelve.", voice, 1.0, 0.0));
    REQUIRE(sink.wait_for(eb::TtsEvent::Started, 10000));
    sink.pump_ms(300);
    tts.stop();
    sink.got.clear();
    Sink::pump_pending();
    sink.got.clear();
    sink.pump_ms(1500);
    CHECK(sink.got.empty());
}

TEST_CASE("tts winrt destruction while speaking is safe")
{
    Sink sink;
    {
        eb::WinRtTts tts;
        tts.set_sink(sink.hwnd, kMsg);
        const auto voice = find_voice(tts, L"en-US");
        if (voice.empty())
            return;
        tts.speak(1, "This sentence is destroyed while it is being spoken.", voice, 1.0, 0.0);
        tts.prepare(2, "Next.", voice, 1.0);
        sink.pump_ms(400);
    }
    sink.pump_ms(300);
    CHECK(true);
}
