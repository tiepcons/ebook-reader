#include "tts/piper/PiperTts.h"

#include "base/Text.h"
#include "tts/piper/PiperProc.h"
#include "tts/Utf8.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Storage.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

namespace eb {

namespace fs = std::filesystem;
using winrt::Windows::Media::Core::MediaSource;
using winrt::Windows::Media::Playback::MediaPlaybackState;
using winrt::Windows::Media::Playback::MediaPlayer;
using winrt::Windows::Storage::StorageFile;

using piper::Cfg;
using piper::Proc;
using piper::Wait;

namespace {

constexpr size_t kMaxVoices = 256;
constexpr size_t kMaxProcs = 2;
constexpr SIZE_T kJobMemory = size_t{1536} * 1024 * 1024;

int rate_key(double rate)
{
    return static_cast<int>(std::lround(std::clamp(rate, 0.5, 3.0) * 20.0));
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
    MediaPlayer playing{nullptr};

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
    void set_playing(MediaPlayer p) noexcept
    {
        const std::scoped_lock lk(pm);
        playing = std::move(p);
    }
    void with_playing(bool play) noexcept
    {
        const std::scoped_lock lk(pm);
        try {
            if (playing) {
                if (play)
                    playing.Play();
                else
                    playing.Pause();
            }
        } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
        }
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
    MediaPlayer player{nullptr};
    fs::path wav;
    Key key;
};

void close_prepared(Prepared& p) noexcept
{
    try {
        if (p.player) {
            p.player.Pause();
            p.player.Close();
        }
    } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
    }
    if (!p.wav.empty()) {
        std::error_code ec;
        fs::remove(p.wav, ec);
    }
    p = Prepared{};
}

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
    Prepared cur;
    Prepared prep;
    uint32_t serial = 0;

    bool make_job() noexcept
    {
        job.reset(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_JOB_MEMORY;
        info.JobMemoryLimit = kJobMemory;
        return job.valid()
            && SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &info, sizeof info);
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
            const fs::path model = voice_model(cfg, voice);
            if (model.empty() || !safe_exe(cfg))
                return nullptr;
            if (procs.size() >= kMaxProcs)
                procs.erase(procs.begin());
            auto p = std::make_unique<Proc>();
            p->voice = voice;
            p->rate = rk;
            if (!p->start(job.get(), cfg, command_line(cfg, model, rk)))
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

enum class Synth : uint8_t { Ok, Cancelled, Failed, BadVoice };

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

Synth run_request(Engine& e, Proc& p, const Req& r, const std::function<bool()>& cancel, fs::path& wav)
{
    wav = e.cfg.temp / (L"eb-" + std::to_wstring(++e.serial) + L".wav");
    const auto utf8 = wide_to_utf8(wav.wstring());
    if (!utf8 || !p.send(piper::request_line(r.text, *utf8)))
        return Synth::Failed;
    const Wait w = p.await(cancel);
    if (w == Wait::Cancelled) {
        p.abandoned.push_back(wav);
        return Synth::Cancelled;
    }
    std::error_code ec;
    return w == Wait::Done && fs::is_regular_file(wav, ec) ? Synth::Ok : Synth::Failed;
}

Synth synth(Engine& e, const Req& r, const std::function<bool()>& cancel, fs::path& wav)
{
    if (!has_content(r.text))
        return Synth::Failed;
    if (voice_model(e.cfg, r.voice).empty())
        return Synth::BadVoice;
    Proc* p = e.acquire(r.voice, r.rate);
    const Synth res = p != nullptr ? run_request(e, *p, r, cancel, wav) : Synth::Failed;
    if (res == Synth::Cancelled)
        return res;
    e.sh->note(r.voice, res == Synth::Ok);
    if (res == Synth::Failed) {
        if (p != nullptr)
            e.drop(p);
        std::error_code ec;
        fs::remove(wav, ec);
    }
    return res;
}

bool build_player(Prepared& p, const fs::path& wav) noexcept
{
    try {
        const StorageFile file = StorageFile::GetFileFromPathAsync(wav.wstring()).get();
        p.player = MediaPlayer();
        p.player.AutoPlay(false);
        p.player.Source(MediaSource::CreateFromStorageFile(file));
        p.wav = wav;
        return true;
    } catch (...) {
        close_prepared(p);
        return false;
    }
}

struct Hook {
    std::shared_ptr<Shared> sh;
    uint32_t gen;
    uint32_t id;
    std::atomic<bool> started{false};

    Hook(std::shared_ptr<Shared> s, uint32_t g, uint32_t i) : sh(std::move(s)), gen(g), id(i) {}

    void start() noexcept
    {
        if (!started.exchange(true))
            sh->post(gen, id, TtsEvent::Started, 0);
    }
};

void hook_player(const std::shared_ptr<Shared>& sh, const Req& r, const MediaPlayer& player)
{
    auto h = std::make_shared<Hook>(sh, r.gen, r.id);
    player.MediaEnded([h](auto&&, auto&&) {
        h->start();
        h->sh->post(h->gen, h->id, TtsEvent::Finished, 0);
    });
    player.MediaFailed([h](auto&&, auto&&) { h->sh->post(h->gen, h->id, TtsEvent::Error, kTtsPlayFailed); });
    player.PlaybackSession().PlaybackStateChanged([h](const auto& session, auto&&) {
        try {
            if (session.PlaybackState() == MediaPlaybackState::Playing)
                h->start();
        } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
        }
    });
}

void close_cur(Engine& e) noexcept
{
    e.sh->set_playing(MediaPlayer{nullptr});
    close_prepared(e.cur);
}

void play_new(Engine& e, Prepared p, const Req& r)
{
    close_cur(e);
    try {
        p.player.Volume(std::clamp(r.volume, 0.0, 1.0));
        hook_player(e.sh, r, p.player);
        e.cur = std::move(p);
        e.sh->set_playing(e.cur.player);
        e.cur.player.Play();
    } catch (...) {
        close_cur(e);
        close_prepared(p);
        e.sh->post(r.gen, r.id, TtsEvent::Error, kTtsPlayFailed);
    }
}

void do_speak(Engine& e, const ReqPtr& req)
{
    if (!e.sh->current(req->gen))
        return;
    close_cur(e);
    Prepared p;
    if (e.prep.player && e.prep.key == key_of(*req)) {
        p = std::move(e.prep);
        e.prep = Prepared{};
    } else {
        fs::path wav;
        const auto cancel = [&] { return e.sh->quit || !e.sh->current(req->gen); };
        const Synth res = synth(e, *req, cancel, wav);
        if (res == Synth::Cancelled)
            return;
        if (res != Synth::Ok) {
            e.sh->post(req->gen, req->id, TtsEvent::Error, res == Synth::BadVoice ? kTtsBadVoice : kTtsSynthFailed);
            return;
        }
        if (!build_player(p, wav)) {
            std::error_code ec;
            fs::remove(wav, ec);
            e.sh->post(req->gen, req->id, TtsEvent::Error, kTtsPlayFailed);
            return;
        }
    }
    if (!e.sh->current(req->gen)) {
        close_prepared(p);
        return;
    }
    p.key = key_of(*req);
    play_new(e, std::move(p), *req);
}

void do_prepare(Engine& e, const ReqPtr& req)
{
    if (!e.sh->epoch_is(req->epoch))
        return;
    close_prepared(e.prep);
    fs::path wav;
    const auto cancel = [&] { return e.sh->quit || !e.sh->epoch_is(req->epoch); };
    if (synth(e, *req, cancel, wav) != Synth::Ok)
        return;
    Prepared p;
    p.key = key_of(*req);
    if (build_player(p, wav) && e.sh->epoch_is(req->epoch)) {
        e.prep = std::move(p);
        return;
    }
    close_prepared(p);
    std::error_code ec;
    fs::remove(wav, ec);
}

void do_stop(Engine& e)
{
    close_cur(e);
    close_prepared(e.prep);
}

}

struct PiperTts::Impl {
    Cfg cfg;
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
        try {
            run_engine();
        } catch (...) { // NOLINT(bugprone-empty-catch): the engine is marked dead below
        }
        sh->dead = true;
        winrt::uninit_apartment();
    }

    void run_engine()
    {
        Engine e;
        e.cfg = cfg;
        e.sh = sh;
        if (e.make_job()) {
            piper::clean_stale(cfg.temp);
            loop(e);
        }
        close_cur(e);
        close_prepared(e.prep);
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
        for (fs::directory_iterator it(impl_->cfg.voices, ec), end; !ec && it != end && out.size() < kMaxVoices;
             it.increment(ec)) {
            const fs::path& p = it->path();
            if (p.extension() != L".onnx")
                continue;
            const std::wstring id = p.stem().wstring();
            if (voice_model(impl_->cfg, id).empty())
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
