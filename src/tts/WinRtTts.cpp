#include "tts/WinRtTts.h"

#include "tts/Utf8.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Media.SpeechSynthesis.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace eb {

using winrt::Windows::Media::Core::MediaSource;
using winrt::Windows::Media::Core::SpeechCue;
using winrt::Windows::Media::Playback::MediaPlaybackItem;
using winrt::Windows::Media::Playback::TimedMetadataTrackPresentationMode;
using winrt::Windows::Media::Playback::MediaPlayer;
using winrt::Windows::Media::SpeechSynthesis::SpeechSynthesizer;

namespace {

constexpr double kMinRate = 0.5;
constexpr double kMaxRate = 3.0;

// Cue events (CueEntered) are used for word boundaries: they fire from the playback clock, so highlighting
// cannot drift from the audio, unlike polling PlaybackSession.Position on a timer.
struct Shared {
    std::mutex m;
    HWND hwnd = nullptr;
    UINT msg = 0;
    uint32_t gen = 0;

    uint32_t bump() noexcept
    {
        const std::scoped_lock lk(m);
        return ++gen;
    }
    bool current(uint32_t g) noexcept
    {
        const std::scoped_lock lk(m);
        return g == gen;
    }
    void post(uint32_t g, uint32_t id, TtsEvent ev, uint32_t value) noexcept
    {
        const std::scoped_lock lk(m);
        if (g == gen && hwnd != nullptr)
            PostMessageW(hwnd, msg, id, pack_tts(ev, value));
    }
};

struct Prepared {
    uint32_t id = 0;
    std::wstring voice;
    double rate = 0;
    MediaPlayer player{nullptr};
    MediaPlaybackItem item{nullptr};
    std::shared_ptr<const std::vector<uint32_t>> map;
};

struct Engine {
    SpeechSynthesizer synth{nullptr};
    Prepared cur;
    Prepared prep;
};

using Task = std::function<void(Engine&)>;

void close_player(Prepared& p) noexcept
{
    try {
        if (p.player) {
            p.player.Pause();
            p.player.Close();
        }
    } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
    }
    p = Prepared{};
}

void to_wide(std::string_view s, std::wstring& w, std::vector<uint32_t>& map)
{
    size_t i = 0;
    while (i < s.size()) {
        const auto c = u8::decode(s, i);
        const auto off = static_cast<uint32_t>(i);
        if (c.cp >= 0x10000) {
            const char32_t v = c.cp - 0x10000;
            w.push_back(static_cast<wchar_t>(0xD800 + (v >> 10)));
            w.push_back(static_cast<wchar_t>(0xDC00 + (v & 0x3FF)));
            map.push_back(off);
            map.push_back(off);
        } else {
            w.push_back(static_cast<wchar_t>(c.cp));
            map.push_back(off);
        }
        i += c.len;
    }
    map.push_back(static_cast<uint32_t>(s.size()));
}

bool select_voice(SpeechSynthesizer& synth, const std::wstring& voice_id)
{
    if (voice_id.empty())
        return true;
    for (const auto& v : SpeechSynthesizer::AllVoices()) {
        if (std::wstring_view(v.Id()) == voice_id) {
            synth.Voice(v);
            return true;
        }
    }
    return false;
}

uint32_t synthesize(Engine& e, Prepared& out, std::string_view text, const std::wstring& voice_id, double rate)
{
    try {
        if (!e.synth)
            e.synth = SpeechSynthesizer();
        if (!select_voice(e.synth, voice_id))
            return kTtsBadVoice;
        std::wstring wide;
        auto map = std::make_shared<std::vector<uint32_t>>();
        to_wide(text, wide, *map);
        if (wide.empty())
            return kTtsSynthFailed;
        auto opt = e.synth.Options();
        opt.IncludeWordBoundaryMetadata(true);
        opt.SpeakingRate(std::clamp(rate, kMinRate, kMaxRate));
        const auto stream = e.synth.SynthesizeTextToStreamAsync(wide).get();
        out.item = MediaPlaybackItem(MediaSource::CreateFromStream(stream, stream.ContentType()));
        out.player = MediaPlayer();
        out.player.AutoPlay(false);
        out.player.Source(out.item);
        out.map = std::move(map);
        out.voice = voice_id;
        out.rate = rate;
        return 0;
    } catch (...) {
        close_player(out);
        return kTtsSynthFailed;
    }
}

void hook_words(const std::shared_ptr<Shared>& sh, uint32_t gen, uint32_t id, Prepared& p)
{
    auto tracks = p.item.TimedMetadataTracks();
    for (uint32_t i = 0; i < tracks.Size(); ++i) {
        const auto track = tracks.GetAt(i);
        if (track.Id() != L"SpeechWord")
            continue;
        tracks.SetPresentationMode(i, TimedMetadataTrackPresentationMode::ApplicationPresented);
        track.CueEntered([sh, gen, id, map = p.map](auto&&, const auto& args) {
            try {
                const auto cue = args.Cue().template try_as<SpeechCue>();
                if (!cue)
                    return;
                const auto pos = cue.StartPositionInInput();
                if (!pos || pos.Value() < 0 || static_cast<size_t>(pos.Value()) >= map->size())
                    return;
                sh->post(gen, id, TtsEvent::Word, (*map)[static_cast<size_t>(pos.Value())]);
            } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
            }
        });
    }
}

void hook_player(const std::shared_ptr<Shared>& sh, uint32_t gen, uint32_t id, Prepared& p)
{
    p.player.MediaEnded([sh, gen, id](auto&&, auto&&) { sh->post(gen, id, TtsEvent::Finished, 0); });
    p.player.MediaFailed([sh, gen, id](auto&&, auto&&) { sh->post(gen, id, TtsEvent::Error, kTtsPlayFailed); });
    hook_words(sh, gen, id, p);
}

struct SpeakReq {
    uint32_t gen;
    uint32_t id;
    std::string text;
    std::wstring voice;
    double rate;
    double volume;
};

void do_speak(Engine& e, const std::shared_ptr<Shared>& sh, const SpeakReq& r)
{
    if (!sh->current(r.gen))
        return;
    close_player(e.cur);
    Prepared p;
    if (e.prep.id == r.id && e.prep.player && e.prep.voice == r.voice && e.prep.rate == r.rate) {
        p = std::move(e.prep);
        e.prep = Prepared{};
    } else if (const uint32_t err = synthesize(e, p, r.text, r.voice, r.rate); err != 0) {
        sh->post(r.gen, r.id, TtsEvent::Error, err);
        return;
    }
    if (!sh->current(r.gen)) {
        close_player(p);
        return;
    }
    try {
        p.player.Volume(std::clamp(r.volume, 0.0, 1.0));
        hook_player(sh, r.gen, r.id, p);
        e.cur = std::move(p);
        sh->post(r.gen, r.id, TtsEvent::Started, 0);
        e.cur.player.Play();
    } catch (...) {
        close_player(e.cur);
        sh->post(r.gen, r.id, TtsEvent::Error, kTtsPlayFailed);
    }
}

void do_prepare(Engine& e, const SpeakReq& r)
{
    close_player(e.prep);
    Prepared p;
    if (synthesize(e, p, r.text, r.voice, r.rate) == 0) {
        p.id = r.id;
        e.prep = std::move(p);
    }
}

}

struct WinRtTts::Impl {
    std::shared_ptr<Shared> sh = std::make_shared<Shared>();
    std::mutex qm;
    std::condition_variable qcv;
    std::deque<Task> q;
    bool quit = false;
    std::thread th;

    void push(Task t) noexcept
    {
        try {
            {
                const std::scoped_lock lk(qm);
                q.push_back(std::move(t));
            }
            qcv.notify_one();
        } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
        }
    }

    void run() noexcept
    {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
        } catch (...) {
            return;
        }
        {
            Engine e;
            for (;;) {
                Task t;
                {
                    std::unique_lock lk(qm);
                    qcv.wait(lk, [&] { return quit || !q.empty(); });
                    if (quit)
                        break;
                    t = std::move(q.front());
                    q.pop_front();
                }
                try {
                    t(e);
                } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
                }
            }
            close_player(e.cur);
            close_player(e.prep);
        }
        winrt::uninit_apartment();
    }
};

WinRtTts::WinRtTts() : impl_(std::make_unique<Impl>())
{
    impl_->th = std::thread([p = impl_.get()] { p->run(); });
}

WinRtTts::~WinRtTts()
{
    impl_->sh->bump();
    {
        const std::scoped_lock lk(impl_->qm);
        impl_->quit = true;
    }
    impl_->qcv.notify_one();
    if (impl_->th.joinable())
        impl_->th.join();
    const std::scoped_lock lk(impl_->sh->m);
    impl_->sh->hwnd = nullptr;
}

std::vector<Voice> WinRtTts::voices()
{
    std::vector<Voice> out;
    try {
        for (const auto& v : SpeechSynthesizer::AllVoices())
            out.push_back({std::wstring(v.Id()), std::wstring(v.DisplayName()), std::wstring(v.Language())});
    } catch (...) {
        out.clear();
    }
    return out;
}

bool WinRtTts::speak(uint32_t id, std::string_view utf8, const std::wstring& voice_id, double rate, double volume) noexcept
{
    try {
        const uint32_t gen = impl_->sh->bump();
        if (utf8.size() > kTtsMaxUtf8) {
            impl_->sh->post(gen, id, TtsEvent::Error, kTtsTextTooLarge);
            return false;
        }
        auto req = std::make_shared<const SpeakReq>(SpeakReq{gen, id, std::string(utf8), voice_id, rate, volume});
        impl_->push([sh = impl_->sh, req](Engine& e) { do_speak(e, sh, *req); });
        return true;
    } catch (...) {
        return false;
    }
}

void WinRtTts::prepare(uint32_t id, std::string_view utf8, const std::wstring& voice_id, double rate) noexcept
{
    try {
        if (utf8.size() > kTtsMaxUtf8)
            return;
        auto req = std::make_shared<const SpeakReq>(SpeakReq{0, id, std::string(utf8), voice_id, rate, 0.0});
        impl_->push([req](Engine& e) { do_prepare(e, *req); });
    } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
    }
}

void WinRtTts::pause() noexcept
{
    impl_->push([](Engine& e) {
        if (e.cur.player)
            e.cur.player.Pause();
    });
}

void WinRtTts::resume() noexcept
{
    impl_->push([](Engine& e) {
        if (e.cur.player)
            e.cur.player.Play();
    });
}

void WinRtTts::stop() noexcept
{
    impl_->sh->bump();
    impl_->push([](Engine& e) {
        close_player(e.cur);
        close_player(e.prep);
    });
}

void WinRtTts::set_sink(HWND hwnd, UINT msg) noexcept
{
    const std::scoped_lock lk(impl_->sh->m);
    impl_->sh->hwnd = hwnd;
    impl_->sh->msg = msg;
}

}
