#include <doctest/doctest.h>

#include "lib/Schema.h"
#include "test_base_tmp.h"
#include "ui/TtsController.h"

#include <cmath>
#include <string>
#include <vector>

namespace {

using eb::TtsEvent;
using eb::TtsKind;
using eb::TtsState;

struct Spoken {
    uint32_t id;
    std::string text;
    std::wstring voice;
};

struct FakeEngine final : eb::TtsEngine {
    std::vector<eb::Voice> list;
    std::vector<Spoken> spoken, prepared;
    int stops = 0, pauses = 0, resumes = 0;
    bool accept = true;
    size_t look = 1;
    double last_volume = -1;

    size_t lookahead() const noexcept override { return look; }
    std::vector<eb::Voice> voices() noexcept override { return list; }
    bool speak(uint32_t id, std::string_view utf8, const std::wstring& voice, double, double volume) noexcept override
    {
        last_volume = volume;
        spoken.push_back({id, std::string(utf8), voice});
        return accept;
    }
    void prepare(uint32_t id, std::string_view utf8, const std::wstring& voice, double) noexcept override
    {
        prepared.push_back({id, std::string(utf8), voice});
    }
    void pause() noexcept override { ++pauses; }
    void resume() noexcept override { ++resumes; }
    void stop() noexcept override { ++stops; }
    void set_sink(HWND, UINT) noexcept override {}
};

struct FakeHost final : eb::TtsHost {
    int32_t pages = 3, shown = 0;
    int64_t resume = -1;
    std::vector<int32_t> requests, turns;
    std::vector<std::pair<uint32_t, uint32_t>> marks;
    std::vector<std::pair<int32_t, uint32_t>> saved;
    std::vector<TtsState> states;
    std::vector<std::wstring> notices;
    int cleared = 0;
    bool right = false;

    void request_text(int32_t page) override { requests.push_back(page); }
    int32_t shown_page() const noexcept override { return shown; }
    int32_t page_count() const noexcept override { return pages; }
    bool shown_has_right_page() const noexcept override { return right; }
    int64_t resume_offset() const noexcept override { return resume; }
    void turn_to(int32_t page) override
    {
        turns.push_back(page);
        shown = page;
    }
    void highlight(int32_t, uint32_t b, uint32_t e) override { marks.emplace_back(b, e); }
    void clear_highlight() override { ++cleared; }
    void save_position(int32_t page, uint32_t off) override { saved.emplace_back(page, off); }
    void state_changed(TtsState s) override { states.push_back(s); }
    void notice(const wchar_t* t) override { notices.emplace_back(t); }
};

LPARAM finished() { return eb::pack_tts(TtsEvent::Finished, 0); }
LPARAM failed(uint32_t code) { return eb::pack_tts(TtsEvent::Error, code); }

struct Rig {
    FakeHost host;
    FakeEngine winrt, piper, azure;
    eb::TtsController ctl{host, {&winrt, nullptr, &azure}, false};
};

}

TEST_CASE("tts controller: sentences are spoken in order, highlighted and recorded")
{
    Rig r;
    r.ctl.start(0, -1);
    REQUIRE(r.host.requests == std::vector<int32_t>{0});
    CHECK(r.ctl.state() == TtsState::Playing);
    r.ctl.on_text(0, "First one. Second one. Third one.");
    REQUIRE(r.winrt.spoken.size() == 1);
    CHECK(r.winrt.spoken[0].text == "First one.");
    CHECK(r.winrt.spoken[0].id == 1);
    REQUIRE(r.winrt.prepared.size() == 1);
    CHECK(r.winrt.prepared[0].text == "Second one.");
    CHECK(r.winrt.prepared[0].id == 2);
    CHECK(r.host.saved.back() == std::pair<int32_t, uint32_t>{0, 0});
    r.ctl.on_event(1, finished());
    REQUIRE(r.winrt.spoken.size() == 2);
    CHECK(r.winrt.spoken[1].text == "Second one.");
    CHECK(r.winrt.spoken[1].id == r.winrt.prepared[0].id);
    CHECK(r.host.saved.back().second == 11);
    CHECK(r.host.marks.back() == std::pair<uint32_t, uint32_t>{11, 22});
}

TEST_CASE("tts controller: an engine that wants a lookahead gets the next sentences announced, across pages too")
{
    Rig r;
    r.winrt.look = 3;
    r.host.pages = 2;
    r.ctl.start(0, -1);
    r.ctl.on_text(0, "One. Two. Three. Four.");
    REQUIRE(r.winrt.prepared.size() == 3);
    CHECK(r.winrt.prepared[0].text == "Two.");
    CHECK(r.winrt.prepared[0].id == 2);
    CHECK(r.winrt.prepared[2].text == "Four.");
    CHECK(r.winrt.prepared[2].id == 4);
    r.ctl.on_event(1, finished());
    REQUIRE(r.winrt.spoken.size() == 2);
    CHECK(r.winrt.spoken[1].id == 2);
    REQUIRE(r.winrt.prepared.size() == 3 + 2);
    CHECK(r.winrt.prepared[3].text == "Three.");
    CHECK(r.winrt.prepared[3].id == 3);
    CHECK(r.host.requests.back() == 1);
    r.ctl.on_text(1, "Five. Six. Seven.");
    REQUIRE(r.winrt.prepared.size() == 3 + 2 + 3);
    CHECK(r.winrt.prepared.back().text == "Five.");
    CHECK(r.winrt.prepared.back().id == 5);
}

TEST_CASE("tts controller: stale, foreign and paused-time events are ignored")
{
    Rig r;
    r.ctl.start(0, -1);
    r.ctl.on_text(0, "One. Two.");
    r.ctl.on_event(99, finished());
    CHECK(r.winrt.spoken.size() == 1);
    r.ctl.toggle();
    CHECK(r.ctl.state() == TtsState::Paused);
    CHECK(r.winrt.pauses == 1);
    r.ctl.on_event(1, finished());
    CHECK(r.winrt.spoken.size() == 1);
    r.ctl.toggle();
    CHECK(r.ctl.state() == TtsState::Playing);
    CHECK(r.winrt.resumes == 1);
    r.ctl.on_event(1, finished());
    CHECK(r.winrt.spoken.size() == 2);
}

TEST_CASE("tts controller: a sentence that ends while paused is spoken on resume")
{
    Rig r;
    r.ctl.start(0, -1);
    r.ctl.on_text(0, "One. Two.");
    r.ctl.toggle();
    r.ctl.on_event(1, finished());
    REQUIRE(r.winrt.spoken.size() == 1);
    r.ctl.toggle();
    REQUIRE(r.winrt.spoken.size() == 2);
    CHECK(r.winrt.spoken[1].text == "Two.");
    CHECK(r.ctl.state() == TtsState::Playing);
}

TEST_CASE("tts controller: a relayout stops reading and restarts at the page shown afterwards")
{
    Rig r;
    r.ctl.start(0, -1);
    r.ctl.on_text(0, "One. Two.");
    r.ctl.relayout();
    CHECK(r.ctl.state() == TtsState::Idle);
    r.ctl.on_text(0, "Stale text.");
    CHECK(r.winrt.spoken.size() == 1);
    r.host.shown = 4;
    r.host.pages = 9;
    r.host.resume = 14;
    r.ctl.relaid();
    CHECK(r.ctl.state() == TtsState::Playing);
    CHECK(r.host.requests.back() == 4);
    r.ctl.on_text(4, "Fresh start. Fresh end.");
    REQUIRE(r.winrt.spoken.size() == 2);
    CHECK(r.winrt.spoken[1].text == "Fresh end.");
    CHECK(r.host.turns.back() == 4);
}

TEST_CASE("tts controller: relaid does nothing when the user stopped meanwhile or nothing was playing")
{
    Rig r;
    r.ctl.relaid();
    CHECK(r.host.requests.empty());
    r.ctl.start(0, -1);
    r.ctl.relayout();
    r.ctl.start(0, -1);
    r.ctl.stop();
    r.ctl.relaid();
    CHECK(r.ctl.state() == TtsState::Idle);
    CHECK(r.host.requests.size() == 2);
}

TEST_CASE("tts controller: a page turn by the user restarts reading at the shown page")
{
    Rig r;
    r.ctl.start(0, -1);
    r.ctl.on_text(0, "One. Two.");
    r.ctl.page_shown();
    CHECK(r.winrt.spoken.size() == 1);
    r.host.shown = 2;
    r.ctl.page_shown();
    CHECK(r.host.requests.back() == 2);
    CHECK(r.ctl.state() == TtsState::Playing);
    r.ctl.on_text(2, "Third page.");
    REQUIRE(r.winrt.spoken.size() == 2);
    CHECK(r.winrt.spoken[1].text == "Third page.");
    CHECK(r.host.turns.back() == 2);
}

TEST_CASE("tts controller: a late text never pulls the view back after a user page turn")
{
    Rig r;
    r.ctl.start(0, -1);
    r.host.shown = 2;
    r.ctl.on_text(0, "Old page.");
    CHECK(r.host.turns.empty());
    CHECK(r.host.requests.back() == 2);
    CHECK(r.winrt.spoken.empty());
    r.ctl.on_text(2, "New page.");
    CHECK(r.winrt.spoken.size() == 1);
    CHECK(r.host.turns.back() == 2);
}

TEST_CASE("tts controller: the last sentence asks for the next page and turns the page when it ends")
{
    Rig r;
    r.ctl.start(0, -1);
    r.ctl.on_text(0, "Alpha. Beta.");
    r.ctl.on_event(1, finished());
    REQUIRE(r.winrt.spoken.size() == 2);
    REQUIRE(r.host.requests.size() == 2);
    CHECK(r.host.requests.back() == 1);
    r.ctl.on_text(1, "Gamma.");
    REQUIRE_FALSE(r.winrt.prepared.empty());
    CHECK(r.winrt.prepared.back().text == "Gamma.");
    CHECK(r.winrt.prepared.back().id == 3);
    r.ctl.on_event(2, finished());
    CHECK(r.host.turns.back() == 1);
    REQUIRE(r.winrt.spoken.size() == 3);
    CHECK(r.winrt.spoken[2].text == "Gamma.");
    CHECK(r.winrt.spoken[2].id == 3);
    CHECK(r.host.saved.back() == std::pair<int32_t, uint32_t>{1, 0});
}

TEST_CASE("tts controller: a page that is not ready yet is waited for")
{
    Rig r;
    r.ctl.start(0, -1);
    r.ctl.on_text(0, "Only.");
    r.ctl.on_event(1, finished());
    CHECK(r.winrt.spoken.size() == 1);
    CHECK(r.ctl.state() == TtsState::Playing);
    r.ctl.on_text(1, "Next page.");
    REQUIRE(r.winrt.spoken.size() == 2);
    CHECK(r.winrt.spoken[1].text == "Next page.");
}

TEST_CASE("tts controller: the end of the book stops and clears the highlight")
{
    Rig r;
    r.host.pages = 1;
    r.ctl.start(0, -1);
    r.ctl.on_text(0, "Last words.");
    r.ctl.on_event(1, finished());
    CHECK(r.ctl.state() == TtsState::Idle);
    CHECK(r.host.cleared >= 1);
    REQUIRE_FALSE(r.host.notices.empty());
    CHECK(r.host.notices.back() == L"Đã đọc hết sách.");
}

TEST_CASE("tts controller: pages without text are skipped, but only a few in a row")
{
    Rig r;
    r.host.pages = 20;
    r.ctl.start(0, -1);
    for (int32_t p = 0; p <= eb::kMaxEmptyPages; ++p) {
        REQUIRE(r.ctl.state() == TtsState::Playing);
        r.ctl.on_text(p, "  \n ");
    }
    CHECK(r.ctl.state() == TtsState::Idle);
    CHECK(r.winrt.spoken.empty());
    CHECK_FALSE(r.host.notices.empty());

    Rig ok;
    ok.host.pages = 20;
    ok.ctl.start(0, -1);
    ok.ctl.on_text(0, "");
    ok.ctl.on_text(1, "Real text.");
    REQUIRE(ok.winrt.spoken.size() == 1);
    CHECK(ok.host.turns.back() == 1);
}

TEST_CASE("tts controller: the saved offset picks the sentence to resume from")
{
    Rig r;
    r.ctl.start(0, 11);
    r.ctl.on_text(0, "First one. Second one. Third one.");
    REQUIRE(r.winrt.spoken.size() == 1);
    CHECK(r.winrt.spoken[0].text == "Second one.");
    CHECK(r.host.saved.back().second == 11);

    Rig beyond;
    beyond.ctl.start(0, 5000);
    beyond.ctl.on_text(0, "First one. Second one.");
    CHECK(beyond.winrt.spoken[0].text == "First one.");

    Rig toggled;
    toggled.host.shown = 4;
    toggled.host.resume = 22;
    toggled.ctl.toggle();
    CHECK(toggled.host.requests == std::vector<int32_t>{4});
    toggled.ctl.on_text(4, "First one. Second one. Third one.");
    CHECK(toggled.winrt.spoken[0].text == "Third one.");
}

TEST_CASE("tts controller: Azure network, quota and auth errors fall back to Windows voices once")
{
    CHECK(eb::azure_should_fall_back(TtsKind::Azure, eb::kTtsNetwork));
    CHECK(eb::azure_should_fall_back(TtsKind::Azure, eb::kTtsQuota));
    CHECK(eb::azure_should_fall_back(TtsKind::Azure, eb::kTtsAuth));
    CHECK_FALSE(eb::azure_should_fall_back(TtsKind::Azure, eb::kTtsBadVoice));
    CHECK_FALSE(eb::azure_should_fall_back(TtsKind::Azure, eb::kTtsSynthFailed));
    CHECK_FALSE(eb::azure_should_fall_back(TtsKind::Piper, eb::kTtsNetwork));
    CHECK_FALSE(eb::azure_should_fall_back(TtsKind::WinRt, eb::kTtsQuota));

    Rig r;
    eb::TtsPrefs p;
    p.engine = TtsKind::Azure;
    r.ctl.set_prefs(p);
    r.ctl.start(0, -1);
    r.ctl.on_text(0, "Hello there. Again.");
    REQUIRE(r.azure.spoken.size() == 1);
    CHECK(r.ctl.active() == TtsKind::Azure);
    r.ctl.on_event(1, failed(eb::kTtsNetwork));
    CHECK(r.ctl.active() == TtsKind::WinRt);
    REQUIRE(r.winrt.spoken.size() == 1);
    CHECK(r.winrt.spoken[0].text == "Hello there.");
    CHECK(r.azure.stops >= 1);
    CHECK(r.ctl.state() == TtsState::Playing);
    REQUIRE_FALSE(r.host.notices.empty());
    r.ctl.on_event(r.winrt.spoken[0].id, failed(eb::kTtsNetwork));
    CHECK(r.ctl.state() == TtsState::Idle);
}

TEST_CASE("tts controller: other errors stop with a message and a missing engine falls back to Windows")
{
    Rig r;
    r.ctl.start(0, -1);
    r.ctl.on_text(0, "One. Two.");
    r.ctl.on_event(1, failed(eb::kTtsBadVoice));
    CHECK(r.ctl.state() == TtsState::Idle);
    CHECK_FALSE(r.host.notices.empty());

    Rig missing;
    eb::TtsPrefs p;
    p.engine = TtsKind::Piper;
    missing.ctl.set_prefs(p);
    missing.ctl.start(0, -1);
    missing.ctl.on_text(0, "One.");
    CHECK(missing.ctl.active() == TtsKind::WinRt);
    CHECK(missing.winrt.spoken.size() == 1);

    Rig refused;
    refused.winrt.accept = false;
    refused.ctl.start(0, -1);
    refused.ctl.on_text(0, "One.");
    CHECK(refused.ctl.state() == TtsState::Idle);
}

TEST_CASE("tts controller: next and previous sentence restart speech with a fresh id")
{
    Rig r;
    r.ctl.start(0, -1);
    r.ctl.on_text(0, "One. Two. Three.");
    r.ctl.step(1);
    REQUIRE(r.winrt.spoken.size() == 2);
    CHECK(r.winrt.spoken[1].text == "Two.");
    CHECK(r.winrt.spoken[1].id == 2);
    r.ctl.step(-1);
    CHECK(r.winrt.spoken[2].text == "One.");
    r.ctl.step(-1);
    CHECK(r.winrt.spoken[3].text == "One.");
    r.ctl.on_event(3, finished());
    CHECK(r.winrt.spoken.size() == 4);
    r.ctl.on_event(4, finished());
    CHECK(r.winrt.spoken[4].text == "Two.");
}

TEST_CASE("tts controller: stop silences the engine and later text for the old page is dropped")
{
    Rig r;
    r.ctl.start(0, -1);
    r.ctl.stop();
    CHECK(r.ctl.state() == TtsState::Idle);
    r.ctl.on_text(0, "Late.");
    CHECK(r.winrt.spoken.empty());
    CHECK(r.winrt.stops >= 1);
}

TEST_CASE("tts controller: voices follow the sentence language")
{
    Rig r;
    r.winrt.list = {{L"en1", L"David", L"en-US"}, {L"vi1", L"An", L"vi-VN"}};
    r.ctl.start(0, -1);
    r.ctl.on_text(0, "This is plain English text. Đây là một câu tiếng Việt khá dài có dấu.");
    REQUIRE(r.winrt.spoken.size() == 1);
    CHECK(r.winrt.spoken[0].voice == L"en1");
    r.ctl.on_event(1, finished());
    REQUIRE(r.winrt.spoken.size() == 2);
    CHECK(r.winrt.spoken[1].voice == L"vi1");
}

TEST_CASE("tts controller: pick_voice")
{
    const std::vector<eb::Voice> list{{L"a", L"A", L"en-US"}, {L"b", L"B", L"vi-VN"}, {L"c", L"C", L"VI_vn"}};
    CHECK(eb::pick_voice(list, L"c", eb::Lang::En) == L"c");
    CHECK(eb::pick_voice(list, L"gone", eb::Lang::Vi) == L"b");
    CHECK(eb::pick_voice(list, L"", eb::Lang::En) == L"a");
    CHECK(eb::pick_voice({}, L"wanted", eb::Lang::En) == L"wanted");
    CHECK(eb::pick_voice({{L"x", L"X", L"fr-FR"}}, L"", eb::Lang::Vi) == L"x");
    CHECK(eb::pick_voice({}, L"", eb::Lang::Vi).empty());
}

TEST_CASE("tts controller: preview speaks a sample only when idle")
{
    Rig r;
    r.ctl.preview(TtsKind::WinRt, L"v", eb::Lang::Vi, 1.0);
    REQUIRE(r.winrt.spoken.size() == 1);
    CHECK(r.winrt.spoken[0].voice == L"v");
    r.ctl.end_preview();
    CHECK(r.winrt.stops >= 1);
    r.ctl.preview(TtsKind::Piper, L"v", eb::Lang::En, 1.0);
    CHECK(r.piper.spoken.empty());
    r.ctl.start(0, -1);
    const auto before = r.winrt.spoken.size();
    r.ctl.preview(TtsKind::WinRt, L"v", eb::Lang::En, 1.0);
    CHECK(r.winrt.spoken.size() == before);
}

TEST_CASE("tts prefs: round trip, clamping and unknown values")
{
    const ebtest::TempDir t;
    auto opened = eb::Db::open(t.path() / L"lib.db");
    REQUIRE(opened.has_value());
    eb::Db db = std::move(*opened);
    REQUIRE(eb::migrate(db).has_value());
    CHECK(eb::load_tts_prefs(db) == eb::TtsPrefs{});
    eb::TtsPrefs p;
    p.engine = TtsKind::Azure;
    p.rate = 1.5;
    p.volume_pct = 35;
    p.voice[2][1] = L"vi-VN-HoaiMyNeural";
    p.voice[0][0] = L"Microsoft David";
    REQUIRE(eb::save_tts_prefs(db, p));
    CHECK(eb::load_tts_prefs(db) == p);
    p.engine = TtsKind::VieNeu;
    p.voice[eb::kind_index(TtsKind::VieNeu)][1] = L"ngoc_huyen";
    REQUIRE(eb::save_tts_prefs(db, p));
    CHECK(eb::load_tts_prefs(db) == p);
    CHECK(eb::clamp_tts_rate(9.0) == eb::kMaxTtsRate);
    CHECK(eb::clamp_tts_rate(0.0) == eb::kMinTtsRate);
    CHECK(eb::clamp_tts_rate(std::nan("")) == 1.0);
    CHECK(eb::load_tts_prefs(db).volume_pct == 35);
    CHECK(eb::clamp_tts_volume_pct(250) == 100);
    CHECK(eb::clamp_tts_volume_pct(-3) == 0);
}

TEST_CASE("tts controller: the saved volume goes to every sentence and changes with the slider")
{
    Rig r;
    eb::TtsPrefs p;
    p.volume_pct = 40;
    r.ctl.set_prefs(p);
    r.ctl.start(0, -1);
    r.ctl.on_text(0, "First one. Second one.");
    CHECK(r.winrt.last_volume == doctest::Approx(0.4));
    r.ctl.set_volume(80);
    CHECK(r.ctl.prefs().volume_pct == 80);
    r.ctl.on_event(1, finished());
    CHECK(r.winrt.last_volume == doctest::Approx(0.8));
    r.ctl.set_volume(500);
    CHECK(r.ctl.prefs().volume_pct == 100);
    r.ctl.set_volume(0); // the live volume is process-wide: keep the engine tests that follow silent
}

TEST_CASE("audio keep-alive: start, pause and stop are idempotent")
{
    eb::AudioKeepAlive k(false);
    static_assert(noexcept(k.start()) && noexcept(k.pause()) && noexcept(k.stop()));
    CHECK_FALSE(k.wanted());
    k.start();
    k.start();
    CHECK(k.wanted());
    k.pause();
    k.pause();
    CHECK_FALSE(k.wanted());
    k.start();
    CHECK(k.wanted());
    k.stop();
    k.stop();
    CHECK_FALSE(k.wanted());
}

TEST_CASE("tts controller: keep-alive runs only while reading")
{
    Rig r;
    CHECK_FALSE(r.ctl.keep_alive_wanted());
    r.ctl.start(0, -1);
    CHECK(r.ctl.keep_alive_wanted());
    r.ctl.on_text(0, "First one. Second one.");
    r.ctl.toggle();
    REQUIRE(r.ctl.state() == TtsState::Paused);
    CHECK_FALSE(r.ctl.keep_alive_wanted());
    r.ctl.toggle();
    REQUIRE(r.ctl.state() == TtsState::Playing);
    CHECK(r.ctl.keep_alive_wanted());
    r.ctl.stop();
    CHECK_FALSE(r.ctl.keep_alive_wanted());
}

TEST_CASE("tts prefs: a saved engine that is not offered yet is not replaced by the first one")
{
    const std::vector<TtsKind> offered{TtsKind::WinRt, TtsKind::Piper};
    CHECK(eb::engine_index(offered, TtsKind::Piper) == 1);
    CHECK_FALSE(eb::engine_index(offered, TtsKind::VieNeu).has_value());
    CHECK_FALSE(eb::engine_index({}, TtsKind::WinRt).has_value());
    CHECK(eb::tts_kind_name(TtsKind::VieNeu) == "vieneu");
}

TEST_CASE("tts controller: a relayout keeps reading from the sentence that was being read")
{
    Rig r;
    r.ctl.start(0, -1);
    r.ctl.on_text(0, "One one one. Two two two. Three three three.");
    r.ctl.on_event(1, finished());
    REQUIRE(r.winrt.spoken.back().text == "Two two two.");
    r.ctl.relayout();
    CHECK(r.ctl.state() == TtsState::Idle);
    r.host.pages = 7;
    r.ctl.relaid();
    CHECK(r.ctl.state() == TtsState::Playing);
    r.ctl.on_text(0, "Zero zero.\nOne one one. Two two\ntwo. Three three three.");
    REQUIRE(r.winrt.spoken.size() == 3);
    CHECK(r.winrt.spoken.back().text == "Two two two.");
}

TEST_CASE("tts controller: repeated relayouts while the text is still awaited neither stall nor lose the place")
{
    Rig r;
    r.ctl.start(0, -1);
    r.ctl.on_text(0, "Aaa aaa. Bbb bbb. Ccc ccc.");
    r.ctl.on_event(1, finished());
    r.ctl.relayout();
    r.ctl.relaid();
    CHECK(r.ctl.state() == TtsState::Playing);
    r.ctl.relayout();
    r.ctl.relayout();
    CHECK(r.ctl.state() == TtsState::Idle);
    r.ctl.relaid();
    r.ctl.on_text(0, "Aaa aaa. Bbb bbb. Ccc ccc.");
    CHECK(r.winrt.spoken.back().text == "Bbb bbb.");
}

TEST_CASE("tts controller: reading can always be started again after a relayout, even without a relaid")
{
    Rig r;
    r.ctl.start(0, -1);
    r.ctl.on_text(0, "One. Two.");
    r.ctl.relayout();
    REQUIRE(r.ctl.state() == TtsState::Idle);
    r.ctl.toggle();
    CHECK(r.ctl.state() == TtsState::Playing);
    r.ctl.on_text(0, "Fresh one. Fresh two.");
    CHECK(r.winrt.spoken.back().text == "Fresh one.");
    r.ctl.relayout();
    r.ctl.relaid();
    r.ctl.relaid();
    r.ctl.on_text(0, "Fresh one. Fresh two.");
    CHECK(r.ctl.state() == TtsState::Playing);
    CHECK(r.winrt.spoken.back().text == "Fresh one.");
}

TEST_CASE("tts controller: a started event after start and resume is accepted without side effects")
{
    Rig r;
    r.ctl.start(0, -1);
    r.ctl.on_text(0, "One. Two.");
    r.ctl.on_event(1, eb::pack_tts(TtsEvent::Started, 0));
    r.ctl.toggle();
    r.ctl.toggle();
    r.ctl.on_event(1, eb::pack_tts(TtsEvent::Started, 0));
    CHECK(r.ctl.state() == TtsState::Playing);
    CHECK(r.winrt.spoken.size() == 1);
}

TEST_CASE("tts controller: among sentences with the same start the one nearest the remembered offset is resumed")
{
    const std::string a = "The very same long opening of this sentence is here for alpha.";
    const std::string b = "The very same long opening of this sentence is here for beta.";
    const std::string text = a + " Middle one. " + b;
    Rig r;
    r.ctl.start(0, -1);
    r.ctl.on_text(0, text);
    r.ctl.relayout();
    r.host.resume = static_cast<int64_t>(a.size() + 13);
    r.ctl.relaid();
    r.ctl.on_text(0, text);
    CHECK(r.winrt.spoken.back().text == b);
}

TEST_CASE("tts controller: a relayout while the right page of a spread is read resumes on that page")
{
    Rig r;
    r.ctl.start(0, -1);
    r.ctl.on_text(0, "Aaa one. Aaa two.");
    r.ctl.on_event(1, finished());
    r.ctl.on_event(2, finished());
    r.ctl.on_text(1, "Bbb one. Bbb two.");
    r.ctl.on_event(3, finished());
    REQUIRE(r.winrt.spoken.back().text == "Bbb two.");
    r.ctl.relayout();
    r.host.shown = 0;
    r.host.right = true;
    r.ctl.relaid();
    r.ctl.on_text(0, "Aaa one. Aaa two.");
    const std::size_t before = r.winrt.spoken.size();
    CHECK(r.host.requests.back() == 1);
    r.ctl.on_text(1, "Bbb one. Bbb two.");
    REQUIRE(r.winrt.spoken.size() == before + 1);
    CHECK(r.winrt.spoken.back().text == "Bbb two.");
    CHECK(r.host.turns.back() == 1);
}

TEST_CASE("tts controller: a snippet found on neither page of the spread restarts at the left page")
{
    Rig r;
    r.ctl.start(0, -1);
    r.ctl.on_text(0, "Aaa one. Aaa two.");
    r.ctl.on_event(1, finished());
    r.ctl.relayout();
    r.host.right = true;
    r.ctl.relaid();
    r.ctl.on_text(0, "Zzz one. Zzz two.");
    r.ctl.on_text(1, "Yyy one. Yyy two.");
    CHECK(r.winrt.spoken.back().text == "Zzz one.");
    CHECK(r.host.turns.back() == 0);
}
