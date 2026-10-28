#include "tts/winrt/WinRtTts.h"

#include "tts/Utf8.h"
#include "tts/winrt/WinRtPlayer.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.SpeechSynthesis.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace eb {

using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::IAsyncOperation;
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
    WinRtPlayer player;
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
    p.player.close();
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

void post_event(Shared& sh, uint32_t gen, uint32_t id, const WordMap& map, PlayEvent ev, uint32_t value) noexcept
{
    switch (ev) {
    case PlayEvent::Started: sh.post(gen, id, TtsEvent::Started, 0); break;
    case PlayEvent::Finished: sh.post(gen, id, TtsEvent::Finished, 0); break;
    case PlayEvent::Failed: sh.post(gen, id, TtsEvent::Error, kTtsPlayFailed); break;
    case PlayEvent::Word:
        if (value < map->size())
            sh.post(gen, id, TtsEvent::Word, (*map)[value]);
        break;
    }
}

void play_new(Engine& e, Prepared p, const Req& r)
{
    Prepared old = std::move(e.cur);
    e.cur = std::move(p);
    const bool ok = e.cur.player.play(r.volume, [sh = e.sh, gen = r.gen, id = r.id, map = e.cur.map](PlayEvent ev, uint32_t v) {
        post_event(*sh, gen, id, map, ev, v);
    });
    if (!ok) {
        close_player(e.cur);
        e.sh->post(r.gen, r.id, TtsEvent::Error, kTtsPlayFailed);
    }
    close_player(old);
}

bool build_player(Prepared& p, const SpeechSynthesisStream& s, WordMap map) noexcept
{
    if (!p.player.open_stream(static_cast<IUnknown*>(winrt::get_abi(s)), s.ContentType().c_str())) {
        close_player(p);
        return false;
    }
    p.map = std::move(map);
    return true;
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
    if (e.prep.player.valid() && e.prep_key == k) {
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
        {
            const WinRtApartment apartment;
            if (apartment.ok()) {
                Engine e;
                e.sh = sh;
                e.queue = queue;
                loop(e);
                close_player(e.cur);
                close_player(e.prep);
            }
            sh->dead = true;
        }
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
        e.cur.player.pause();
    });
}

void WinRtTts::resume() noexcept
{
    impl_->queue->push([](Engine& e) {
        e.cur.player.resume();
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
