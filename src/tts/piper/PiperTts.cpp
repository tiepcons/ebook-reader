#include "tts/piper/PiperTts.h"

#include "base/Text.h"
#include "tts/Utf8.h"
#include "tts/piper/PiperProc.h"
#include "tts/piper/player/PiperPlayer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <thread>

namespace eb {

namespace fs = std::filesystem;

using piper::Cancel;
using piper::Cfg;
using piper::Player;
using piper::PlayEvent;
using piper::Proc;
using piper::Wait;

namespace {

constexpr size_t kMaxVoices = 256;
constexpr size_t kMaxVoiceEntries = 4096;
constexpr size_t kMaxProcs = 2;
constexpr SIZE_T kJobMemory = size_t{1536} * 1024 * 1024;
constexpr auto kBaseLimit = std::chrono::seconds(30);
constexpr auto kMaxLimit = std::chrono::seconds(300);

int rate_key(double rate)
{
    return static_cast<int>(std::lround(std::clamp(rate, 0.5, 3.0) * 20.0));
}

std::chrono::seconds synth_limit(size_t utf8_bytes)
{
    return std::min(kBaseLimit + std::chrono::seconds(utf8_bytes / 200), kMaxLimit);
}

struct Shared {
    std::mutex m;
    HWND hwnd = nullptr;
    UINT msg = 0;
    uint32_t gen = 0;
    uint32_t epoch = 0;
    std::atomic<bool> dead{false};
    std::atomic<bool> quit{false};
    std::map<std::wstring, int> fails;
    std::mutex pm;
    Player playing;

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
    void invalidate_all() noexcept
    {
        const std::scoped_lock lk(m);
        ++gen;
        ++epoch;
    }
    void post(uint32_t g, uint32_t id, TtsEvent ev, uint32_t value) noexcept
    {
        const std::scoped_lock lk(m);
        if (g == gen && hwnd != nullptr)
            PostMessageW(hwnd, msg, id, pack_tts(ev, value));
    }
    bool voice_dead(const std::wstring& v) noexcept
    {
        try {
            const std::scoped_lock lk(m);
            const auto it = fails.find(v);
            return it != fails.end() && it->second >= 2;
        } catch (...) {
            return true;
        }
    }
    void note(const std::wstring& v, bool ok) noexcept
    {
        try {
            const std::scoped_lock lk(m);
            int& n = fails[v];
            n = ok ? 0 : n + 1;
        } catch (...) { // NOLINT(bugprone-empty-catch): the counter is advisory
        }
    }
    void set_playing(Player p) noexcept
    {
        const std::scoped_lock lk(pm);
        playing = std::move(p);
    }
    void with_playing(bool play) noexcept
    {
        Player p;
        {
            const std::scoped_lock lk(pm);
            p = playing;
        }
        if (play)
            p.resume();
        else
            p.pause();
    }
};

struct Req {
    uint32_t gen = 0;
    uint32_t epoch = 0;
    uint32_t id = 0;
    std::string text;
    std::wstring voice;
    int rate = 0;
    double volume = 0.0;
};
using ReqPtr = std::shared_ptr<const Req>;

struct Key {
    uint32_t id = 0;
    size_t hash = 0;
    std::wstring voice;
    int rate = 0;
    bool operator==(const Key&) const = default;
};

Key key_of(const Req& r)
{
    return {r.id, std::hash<std::string_view>{}(r.text), r.voice, r.rate};
}

struct Prepared {
    Player player;
    fs::path wav;
    Key key;
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
    Cfg cfg;
    std::shared_ptr<Shared> sh;
    UniqueHandle job;
    std::vector<std::unique_ptr<Proc>> procs;
    std::vector<fs::path> pending;
    Prepared cur;
    Prepared prep;
    uint32_t serial = 0;

    bool make_job() noexcept
    {
        job.reset(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_JOB_MEMORY
            | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        info.BasicLimitInformation.ActiveProcessLimit = kMaxProcs;
        info.JobMemoryLimit = kJobMemory;
        return job.valid() && SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &info, sizeof info);
    }

    // A WAV that cannot be deleted yet (still open elsewhere) is retried on the next request and at shutdown.
    void discard(const fs::path& p) noexcept
    {
        if (p.empty())
            return;
        try {
            std::error_code ec;
            fs::remove(p, ec);
            if (ec)
                pending.push_back(p);
        } catch (...) { // NOLINT(bugprone-empty-catch): stale files are removed at the next start
        }
    }

    void retry_pending() noexcept
    {
        std::vector<fs::path> again;
        again.swap(pending);
        for (const auto& p : again)
            discard(p);
    }

    Proc* acquire(const std::wstring& voice, int rk) noexcept
    {
        try {
            const auto it = std::ranges::find_if(procs, [&](const auto& p) { return p->voice == voice; });
            if (it != procs.end()) {
                if ((*it)->rate == rk && (*it)->alive()) {
                    std::rotate(it, it + 1, procs.end());
                    return procs.back().get();
                }
                procs.erase(it);
            }
            const fs::path model = piper::voice_model(cfg, voice);
            if (model.empty() || !piper::safe_exe(cfg))
                return nullptr;
            if (procs.size() >= kMaxProcs)
                procs.erase(procs.begin());
            auto p = std::make_unique<Proc>();
            p->voice = voice;
            p->rate = rk;
            if (!p->start(job.get(), cfg, piper::command_line(cfg, model, rk)))
                return nullptr;
            procs.push_back(std::move(p));
            return procs.back().get();
        } catch (...) {
            return nullptr;
        }
    }

    void drop(Proc* p) noexcept
    {
        try {
            std::erase_if(procs, [&](const auto& q) { return q.get() == p; });
        } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
        }
    }
};

void close_prepared(Engine& e, Prepared& p) noexcept
{
    p.player.close();
    e.discard(p.wav);
    p = Prepared{};
}

void close_cur(Engine& e) noexcept
{
    e.sh->set_playing(Player{});
    close_prepared(e, e.cur);
}

enum class Synth : uint8_t { Ok, Cancelled, TimedOut, Failed, BadVoice };

bool has_content(std::string_view s)
{
    size_t i = 0;
    while (i < s.size()) {
        const auto c = u8::decode(s, i);
        if (!u8::is_space(c.cp))
            return true;
        i += c.len;
    }
    return false;
}

Synth run_request(Engine& e, Proc& p, const Req& r, const Cancel& cancel, fs::path& wav)
{
    wav = e.cfg.temp / (L"eb-" + std::to_wstring(++e.serial) + L".wav");
    const auto utf8 = wide_to_utf8(wav.wstring());
    if (!utf8 || !p.send(piper::request_line(r.text, *utf8), cancel))
        return cancel() ? Synth::Cancelled : Synth::Failed;
    switch (p.await(cancel, synth_limit(r.text.size()))) {
    case Wait::Done: {
        std::error_code ec;
        return fs::is_regular_file(wav, ec) ? Synth::Ok : Synth::Failed;
    }
    case Wait::Cancelled: return Synth::Cancelled;
    case Wait::Timeout: return Synth::TimedOut;
    case Wait::Dead: break;
    }
    return Synth::Failed;
}

// A cancelled, timed-out or failed child is killed and dropped; its half-written WAV is deleted afterwards.
// Timeouts and cancellations do not count against the voice.
Synth synth(Engine& e, const Req& r, const Cancel& cancel, fs::path& wav)
{
    e.retry_pending();
    if (!has_content(r.text))
        return Synth::Failed;
    if (piper::voice_model(e.cfg, r.voice).empty())
        return Synth::BadVoice;
    Proc* p = e.acquire(r.voice, r.rate);
    const Synth res = p != nullptr ? run_request(e, *p, r, cancel, wav) : Synth::Failed;
    if (res != Synth::Ok) {
        if (p != nullptr)
            e.drop(p);
        e.discard(wav);
    }
    if (res == Synth::Ok || res == Synth::Failed)
        e.sh->note(r.voice, res == Synth::Ok);
    return res;
}

void play_new(Engine& e, Prepared p, const Req& r)
{
    close_cur(e);
    const bool ok = p.player.play(r.volume, [sh = e.sh, gen = r.gen, id = r.id](PlayEvent ev) {
        switch (ev) {
        case PlayEvent::Started: sh->post(gen, id, TtsEvent::Started, 0); break;
        case PlayEvent::Finished: sh->post(gen, id, TtsEvent::Finished, 0); break;
        case PlayEvent::Failed: sh->post(gen, id, TtsEvent::Error, kTtsPlayFailed); break;
        }
    });
    if (!ok) {
        close_prepared(e, p);
        e.sh->post(r.gen, r.id, TtsEvent::Error, kTtsPlayFailed);
        return;
    }
    e.cur = std::move(p);
    e.sh->set_playing(e.cur.player);
}

bool fetch_wav(Engine& e, const ReqPtr& req, Prepared& p)
{
    fs::path wav;
    const Cancel cancel = [&] { return e.sh->quit || !e.sh->current(req->gen); };
    const Synth res = synth(e, *req, cancel, wav);
    if (res == Synth::Cancelled)
        return false;
    if (res != Synth::Ok) {
        e.sh->post(req->gen, req->id, TtsEvent::Error, res == Synth::BadVoice ? kTtsBadVoice : kTtsSynthFailed);
        return false;
    }
    p.wav = wav;
    if (!p.player.open(wav)) {
        close_prepared(e, p);
        e.sh->post(req->gen, req->id, TtsEvent::Error, kTtsPlayFailed);
        return false;
    }
    return true;
}

void do_speak(Engine& e, const ReqPtr& req)
{
    if (!e.sh->current(req->gen))
        return;
    close_cur(e);
    Prepared p;
    if (e.prep.player.valid() && e.prep.key == key_of(*req)) {
        p = std::move(e.prep);
        e.prep = Prepared{};
    } else if (!fetch_wav(e, req, p)) {
        return;
    }
    if (!e.sh->current(req->gen)) {
        close_prepared(e, p);
        return;
    }
    p.key = key_of(*req);
    play_new(e, std::move(p), *req);
}

void do_prepare(Engine& e, const ReqPtr& req)
{
    if (!e.sh->epoch_is(req->epoch))
        return;
    close_prepared(e, e.prep);
    fs::path wav;
    const Cancel cancel = [&] { return e.sh->quit || !e.sh->epoch_is(req->epoch); };
    if (synth(e, *req, cancel, wav) != Synth::Ok)
        return;
    Prepared p;
    p.key = key_of(*req);
    p.wav = wav;
    if (p.player.open(wav) && e.sh->epoch_is(req->epoch))
        e.prep = std::move(p);
    else
        close_prepared(e, p);
}

void do_stop(Engine& e)
{
    close_cur(e);
    close_prepared(e, e.prep);
}

}

struct PiperTts::Impl {
    Cfg cfg;
    std::shared_ptr<Shared> sh = std::make_shared<Shared>();
    std::shared_ptr<Queue> queue = std::make_shared<Queue>();
    std::thread th;

    void run() noexcept
    {
        {
            const piper::Apartment apartment;
            if (apartment.ok()) {
                try {
                    run_engine();
                } catch (...) { // NOLINT(bugprone-empty-catch): the engine is marked dead below
                }
            }
        }
        sh->dead = true;
    }

    void run_engine()
    {
        Engine e;
        e.cfg = cfg;
        e.sh = sh;
        if (e.make_job() && piper::prepare_temp_dir(cfg.temp))
            loop(e);
        close_cur(e);
        close_prepared(e, e.prep);
        e.retry_pending();
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

PiperTts::PiperTts(const fs::path& piper_dir, const fs::path& voices_dir, const fs::path& temp_dir)
    : impl_(std::make_unique<Impl>())
{
    std::error_code ec;
    impl_->cfg = {fs::absolute(piper_dir, ec), fs::absolute(voices_dir, ec), fs::absolute(temp_dir, ec)};
    impl_->th = std::thread([p = impl_.get()] { p->run(); });
}

PiperTts::~PiperTts()
{
    impl_->sh->quit = true;
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

std::vector<Voice> PiperTts::voices() noexcept
{
    std::vector<Voice> out;
    try {
        std::error_code ec;
        if (!fs::is_regular_file(impl_->cfg.exe(), ec))
            return out;
        size_t seen = 0;
        for (fs::directory_iterator it(impl_->cfg.voices, ec), end;
             !ec && it != end && out.size() < kMaxVoices && seen++ < kMaxVoiceEntries; it.increment(ec)) {
            const fs::path& p = it->path();
            if (p.extension() != L".onnx" || piper::is_reparse(p))
                continue;
            const std::wstring id = p.stem().wstring();
            if (piper::voice_model(impl_->cfg, id).empty())
                continue;
            out.push_back({id, id, piper::voice_lang(p)});
        }
    } catch (...) {
        out.clear();
    }
    return out;
}

bool PiperTts::speak(uint32_t id, std::string_view utf8, const std::wstring& voice_id, double rate, double volume) noexcept
{
    try {
        const uint32_t gen = impl_->sh->bump_gen();
        const bool too_big = utf8.size() > kTtsMaxUtf8;
        if (impl_->sh->dead || too_big || impl_->sh->voice_dead(voice_id)) {
            impl_->sh->post(gen, id, TtsEvent::Error, too_big ? kTtsTextTooLarge : kTtsSynthFailed);
            return false;
        }
        auto req = std::make_shared<const Req>(Req{gen, 0, id, std::string(utf8), voice_id, rate_key(rate), volume});
        impl_->queue->push([req](Engine& e) { do_speak(e, req); });
        return true;
    } catch (...) {
        return false;
    }
}

void PiperTts::prepare(uint32_t id, std::string_view utf8, const std::wstring& voice_id, double rate) noexcept
{
    try {
        if (utf8.size() > kTtsMaxUtf8 || impl_->sh->dead || impl_->sh->voice_dead(voice_id))
            return;
        auto req = std::make_shared<const Req>(
            Req{0, impl_->sh->epoch_now(), id, std::string(utf8), voice_id, rate_key(rate), 0.0});
        impl_->queue->push([req](Engine& e) { do_prepare(e, req); });
    } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
    }
}

void PiperTts::pause() noexcept
{
    impl_->sh->with_playing(false);
}

void PiperTts::resume() noexcept
{
    impl_->sh->with_playing(true);
}

void PiperTts::stop() noexcept
{
    impl_->sh->invalidate_all();
    impl_->queue->push([](Engine& e) { do_stop(e); });
}

void PiperTts::set_sink(HWND hwnd, UINT msg) noexcept
{
    const std::scoped_lock lk(impl_->sh->m);
    impl_->sh->hwnd = hwnd;
    impl_->sh->msg = msg;
}

}
