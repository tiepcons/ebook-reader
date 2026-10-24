#include "tts/winrt/WinRtTts.h"

#include "tts/Utf8.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Media.SpeechSynthesis.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <thread>

namespace eb {

using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::IAsyncOperation;
using winrt::Windows::Media::Core::MediaSource;
using winrt::Windows::Media::Core::SpeechCue;
using winrt::Windows::Media::Playback::MediaPlaybackItem;
using winrt::Windows::Media::Playback::MediaPlaybackState;
using winrt::Windows::Media::Playback::MediaPlayer;
using winrt::Windows::Media::Playback::TimedMetadataTrackPresentationMode;
using winrt::Windows::Media::SpeechSynthesis::SpeechSynthesisStream;
using winrt::Windows::Media::SpeechSynthesis::SpeechSynthesizer;

namespace {

constexpr double kMinRate = 0.5;
constexpr double kMaxRate = 3.0;

using SynthOp = IAsyncOperation<SpeechSynthesisStream>;
using WordMap = std::shared_ptr<const std::vector<uint32_t>>;

void cancel_op(SynthOp& op) noexcept
{
    try {
        if (op)
            op.Cancel();
    } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
    }
    op = nullptr;
}

struct Shared {
    std::mutex m;
    HWND hwnd = nullptr;
    UINT msg = 0;
    uint32_t gen = 0;
    uint32_t epoch = 0;
    std::atomic<bool> dead{false};
    std::mutex om;
    SynthOp speak_op{nullptr};
    SynthOp prep_op{nullptr};

    bool current(uint32_t g) noexcept
    {
        const std::scoped_lock lk(m);
        return g == gen;
    }
    bool epoch_is(uint32_t e) noexcept
    {
        const std::scoped_lock lk(m);
        return e == epoch;
    }
    uint32_t epoch_now() noexcept
    {
        const std::scoped_lock lk(m);
        return epoch;
    }
    uint32_t bump_gen() noexcept
    {
        const std::scoped_lock lk(m);
        return ++gen;
    }
    uint32_t begin_speak() noexcept
    {
        const std::scoped_lock ol(om);
        const uint32_t g = bump_gen();
        cancel_op(speak_op);
        return g;
    }
    void invalidate_all() noexcept
    {
        const std::scoped_lock ol(om);
        {
            const std::scoped_lock lk(m);
            ++gen;
            ++epoch;
        }
        cancel_op(speak_op);
        cancel_op(prep_op);
    }
    void post(uint32_t g, uint32_t id, TtsEvent ev, uint32_t value) noexcept
    {
        const std::scoped_lock lk(m);
        if (g == gen && hwnd != nullptr)
            PostMessageW(hwnd, msg, id, pack_tts(ev, value));
    }
};

struct Req {
    uint32_t gen = 0;
    uint32_t epoch = 0;
    uint32_t id = 0;
    std::string text;
    std::wstring voice;
    double rate = 1.0;
    double volume = 0.0;
};
using ReqPtr = std::shared_ptr<const Req>;

struct Key {
    uint32_t id = 0;
    size_t hash = 0;
    std::wstring voice;
    double rate = 0;
    bool operator==(const Key&) const = default;
};

Key key_of(const Req& r)
{
    return {r.id, std::hash<std::string_view>{}(r.text), r.voice, r.rate};
}

struct Prepared {
    MediaPlayer player{nullptr};
    MediaPlaybackItem item{nullptr};
    WordMap map;
};

struct Engine;
using Task = std::function<void(Engine&)>;

struct Queue {
    std::mutex m;
    std::condition_variable cv;
    std::deque<Task> q;
    bool quit = false;

    void push(Task t) noexcept
    {
        try {
            {
                const std::scoped_lock lk(m);
                q.push_back(std::move(t));
            }
            cv.notify_one();
        } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
        }
    }
};

struct Engine {
    std::shared_ptr<Shared> sh;
    std::shared_ptr<Queue> queue;
    Prepared cur;
    Prepared prep;
    Key prep_key;
    bool prep_pending = false;
    Key pending_key;
    uint32_t speak_serial = 0;
    uint32_t prep_serial = 0;
    ReqPtr waiting;
};

enum class Kind : uint8_t { Speak, Prepare };

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

// Word boundaries come from the SpeechWord track's CueEntered event: it fires from the playback clock, so
// highlighting cannot drift from the audio, unlike polling PlaybackSession.Position on a timer.
struct Hook {
    std::shared_ptr<Shared> sh;
    uint32_t gen;
    uint32_t id;
    WordMap map;
    std::atomic<bool> started{false};
    std::mutex m;
    std::set<uint32_t> hooked;

    Hook(std::shared_ptr<Shared> s, uint32_t g, uint32_t i, WordMap w) : sh(std::move(s)), gen(g), id(i), map(std::move(w)) {}

    void start() noexcept
    {
        if (!started.exchange(true))
            sh->post(gen, id, TtsEvent::Started, 0);
    }
    void word(int32_t pos) noexcept
    {
        if (pos < 0 || static_cast<size_t>(pos) >= map->size())
            return;
        start();
        sh->post(gen, id, TtsEvent::Word, (*map)[static_cast<size_t>(pos)]);
    }
};

void on_cue(const std::shared_ptr<Hook>& h, const winrt::Windows::Media::Core::IMediaCue& c) noexcept
{
    try {
        const auto cue = c.try_as<SpeechCue>();
        if (!cue)
            return;
        const auto pos = cue.StartPositionInInput();
        if (pos)
            h->word(pos.Value());
    } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
    }
}

void hook_tracks(const std::shared_ptr<Hook>& h, const MediaPlaybackItem& item)
{
    const std::scoped_lock lk(h->m);
    auto tracks = item.TimedMetadataTracks();
    for (uint32_t i = 0; i < tracks.Size(); ++i) {
        if (!h->hooked.insert(i).second)
            continue;
        const auto track = tracks.GetAt(i);
        if (track.Id() != L"SpeechWord")
            continue;
        tracks.SetPresentationMode(i, TimedMetadataTrackPresentationMode::ApplicationPresented);
        track.CueEntered([h](auto&&, const auto& args) { on_cue(h, args.Cue()); });
    }
}

void hook_player(const std::shared_ptr<Shared>& sh, const Req& r, const Prepared& p)
{
    auto h = std::make_shared<Hook>(sh, r.gen, r.id, p.map);
    p.player.MediaEnded([h](auto&&, auto&&) {
        h->start();
        h->sh->post(h->gen, h->id, TtsEvent::Finished, 0);
    });
    p.player.MediaFailed([h](auto&&, auto&&) { h->sh->post(h->gen, h->id, TtsEvent::Error, kTtsPlayFailed); });
    p.player.PlaybackSession().PlaybackStateChanged([h](const auto& session, auto&&) {
        try {
            if (session.PlaybackState() == MediaPlaybackState::Playing)
                h->start();
        } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
        }
    });
    p.item.TimedMetadataTracksChanged([h](const MediaPlaybackItem& it, auto&&) {
        try {
            hook_tracks(h, it);
        } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
        }
    });
    hook_tracks(h, p.item);
}

void play_new(Engine& e, Prepared p, const Req& r)
{
    try {
        p.player.Volume(std::clamp(r.volume, 0.0, 1.0));
        hook_player(e.sh, r, p);
    } catch (...) {
        close_player(p);
        e.sh->post(r.gen, r.id, TtsEvent::Error, kTtsPlayFailed);
        return;
    }
    Prepared old = std::move(e.cur);
    e.cur = std::move(p);
    try {
        e.cur.player.Play();
    } catch (...) {
        close_player(e.cur);
        e.sh->post(r.gen, r.id, TtsEvent::Error, kTtsPlayFailed);
    }
    close_player(old);
}

bool build_player(Prepared& p, const SpeechSynthesisStream& s, WordMap map) noexcept
{
    try {
        p.item = MediaPlaybackItem(MediaSource::CreateFromStream(s, s.ContentType()));
        p.player = MediaPlayer();
        p.player.AutoPlay(false);
        p.player.Source(p.item);
        p.map = std::move(map);
        return true;
    } catch (...) {
        close_player(p);
        return false;
    }
}

void on_synth_done(Engine& e, Kind kind, uint32_t serial, const SpeechSynthesisStream& stream, const WordMap& map,
    const ReqPtr& req);

void completed(const std::shared_ptr<Queue>& q, Kind kind, uint32_t serial, const WordMap& map, const ReqPtr& req,
    const SynthOp& op, AsyncStatus st) noexcept
{
    try {
        if (st == AsyncStatus::Canceled)
            return;
        SpeechSynthesisStream stream{nullptr};
        if (st == AsyncStatus::Completed) {
            try {
                stream = op.GetResults();
            } catch (...) { // NOLINT(bugprone-empty-catch): a null stream is reported as an error
            }
        }
        q->push([=](Engine& e) { on_synth_done(e, kind, serial, stream, map, req); });
    } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
    }
}

bool register_op(Shared& sh, Kind kind, const Req& r, const SynthOp& op)
{
    const std::scoped_lock lk(sh.om);
    const bool valid = kind == Kind::Speak ? sh.current(r.gen) : sh.epoch_is(r.epoch);
    if (!valid)
        return false;
    SynthOp& slot = kind == Kind::Speak ? sh.speak_op : sh.prep_op;
    cancel_op(slot);
    slot = op;
    return true;
}

uint32_t start_synth(Engine& e, Kind kind, const ReqPtr& req, uint32_t serial) noexcept
{
    try {
        SpeechSynthesizer synth;
        if (!select_voice(synth, req->voice))
            return kTtsBadVoice;
        std::wstring wide;
        auto map = std::make_shared<std::vector<uint32_t>>();
        to_wide(req->text, wide, *map);
        if (wide.empty())
            return kTtsSynthFailed;
        auto opt = synth.Options();
        opt.IncludeWordBoundaryMetadata(true);
        opt.SpeakingRate(std::clamp(req->rate, kMinRate, kMaxRate));
        auto op = synth.SynthesizeTextToStreamAsync(wide);
        if (!register_op(*e.sh, kind, *req, op)) {
            cancel_op(op);
            return 0;
        }
        op.Completed([q = e.queue, kind, serial, map = WordMap(map), req, synth](const SynthOp& o, AsyncStatus st) {
            completed(q, kind, serial, map, req, o, st);
        });
        return 0;
    } catch (...) {
        return kTtsSynthFailed;
    }
}

void begin_speak_synth(Engine& e, const ReqPtr& req)
{
    if (const uint32_t err = start_synth(e, Kind::Speak, req, e.speak_serial); err != 0)
        e.sh->post(req->gen, req->id, TtsEvent::Error, err);
}

void on_synth_done(Engine& e, Kind kind, uint32_t serial, const SpeechSynthesisStream& stream, const WordMap& map,
    const ReqPtr& req)
{
    Prepared p;
    const bool ok = stream && build_player(p, stream, map);
    if (kind == Kind::Speak) {
        if (serial != e.speak_serial || !e.sh->current(req->gen))
            return;
        if (ok)
            play_new(e, std::move(p), *req);
        else
            e.sh->post(req->gen, req->id, TtsEvent::Error, kTtsSynthFailed);
        return;
    }
    if (serial != e.prep_serial)
        return;
    e.prep_pending = false;
    const auto w = e.waiting;
    const bool match = w && key_of(*w) == e.pending_key;
    if (!e.sh->epoch_is(req->epoch) || !ok) {
        if (match && e.sh->current(w->gen))
            e.sh->post(w->gen, w->id, TtsEvent::Error, kTtsSynthFailed);
        return;
    }
    if (match && e.sh->current(w->gen)) {
        e.waiting.reset();
        play_new(e, std::move(p), *w);
        return;
    }
    close_player(e.prep);
    e.prep = std::move(p);
    e.prep_key = key_of(*req);
}

void do_speak(Engine& e, const ReqPtr& req)
{
    if (!e.sh->current(req->gen))
        return;
    e.waiting.reset();
    ++e.speak_serial;
    const Key k = key_of(*req);
    if (e.prep.player && e.prep_key == k) {
        Prepared p = std::move(e.prep);
        e.prep = Prepared{};
        play_new(e, std::move(p), *req);
        return;
    }
    close_player(e.cur);
    if (e.prep_pending && e.pending_key == k)
        e.waiting = req;
    else
        begin_speak_synth(e, req);
}

void do_prepare(Engine& e, const ReqPtr& req)
{
    if (!e.sh->epoch_is(req->epoch))
        return;
    const Key k = key_of(*req);
    if (e.waiting && !(key_of(*e.waiting) == k)) {
        const auto w = std::move(e.waiting);
        e.waiting.reset();
        begin_speak_synth(e, w);
    }
    close_player(e.prep);
    e.pending_key = k;
    e.prep_pending = start_synth(e, Kind::Prepare, req, ++e.prep_serial) == 0;
}

void do_stop(Engine& e)
{
    ++e.speak_serial;
    ++e.prep_serial;
    e.prep_pending = false;
    e.waiting.reset();
    close_player(e.cur);
    close_player(e.prep);
}

}

struct WinRtTts::Impl {
    std::shared_ptr<Shared> sh = std::make_shared<Shared>();
    std::shared_ptr<Queue> queue = std::make_shared<Queue>();
    std::thread th;

    void run() noexcept
    {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
        } catch (...) {
            sh->dead = true;
            return;
        }
        {
            Engine e;
            e.sh = sh;
            e.queue = queue;
            loop(e);
            close_player(e.cur);
            close_player(e.prep);
        }
        sh->dead = true;
        winrt::uninit_apartment();
    }

    void loop(Engine& e) noexcept
    {
        for (;;) {
            Task t;
            {
                std::unique_lock lk(queue->m);
                queue->cv.wait(lk, [&] { return queue->quit || !queue->q.empty(); });
                if (queue->quit)
                    return;
                t = std::move(queue->q.front());
                queue->q.pop_front();
            }
            try {
                t(e);
            } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
            }
        }
    }
};

WinRtTts::WinRtTts() : impl_(std::make_unique<Impl>())
{
    impl_->th = std::thread([p = impl_.get()] { p->run(); });
}

WinRtTts::~WinRtTts()
{
    impl_->sh->invalidate_all();
    {
        const std::scoped_lock lk(impl_->queue->m);
        impl_->queue->quit = true;
    }
    impl_->queue->cv.notify_one();
    if (impl_->th.joinable())
        impl_->th.join();
    const std::scoped_lock lk(impl_->sh->m);
    impl_->sh->hwnd = nullptr;
}

std::vector<Voice> WinRtTts::voices() noexcept
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
        const uint32_t gen = impl_->sh->begin_speak();
        if (impl_->sh->dead || utf8.size() > kTtsMaxUtf8) {
            impl_->sh->post(gen, id, TtsEvent::Error, impl_->sh->dead ? kTtsSynthFailed : kTtsTextTooLarge);
            return false;
        }
        auto req = std::make_shared<const Req>(Req{gen, 0, id, std::string(utf8), voice_id, rate, volume});
        impl_->queue->push([req](Engine& e) { do_speak(e, req); });
        return true;
    } catch (...) {
        return false;
    }
}

void WinRtTts::prepare(uint32_t id, std::string_view utf8, const std::wstring& voice_id, double rate) noexcept
{
    try {
        if (utf8.size() > kTtsMaxUtf8 || impl_->sh->dead)
            return;
        auto req = std::make_shared<const Req>(Req{0, impl_->sh->epoch_now(), id, std::string(utf8), voice_id, rate, 0.0});
        impl_->queue->push([req](Engine& e) { do_prepare(e, req); });
    } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
    }
}

void WinRtTts::pause() noexcept
{
    impl_->queue->push([](Engine& e) {
        if (e.cur.player)
            e.cur.player.Pause();
    });
}

void WinRtTts::resume() noexcept
{
    impl_->queue->push([](Engine& e) {
        if (e.cur.player)
            e.cur.player.Play();
    });
}

void WinRtTts::stop() noexcept
{
    impl_->sh->invalidate_all();
    impl_->queue->push([](Engine& e) { do_stop(e); });
}

void WinRtTts::set_sink(HWND hwnd, UINT msg) noexcept
{
    const std::scoped_lock lk(impl_->sh->m);
    impl_->sh->hwnd = hwnd;
    impl_->sh->msg = msg;
}

}
