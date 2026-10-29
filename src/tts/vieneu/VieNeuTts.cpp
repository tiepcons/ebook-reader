#include "tts/vieneu/VieNeuTts.h"

#include "tts/vieneu/VieNeuEngine.h"

#include <thread>

namespace eb {

namespace fs = std::filesystem;

namespace vieneu::engine {
namespace {

constexpr size_t kLookahead = 4;
constexpr DWORD kMaxProcs = 1;

// audiocpp_cli commits about 9 GB (reserved ggml contexts; no session option shrinks it, its working set stays near
// 800 MB). The cap must clear that, but never exceeds half of the commit limit (RAM + page file) of the machine.
SIZE_T job_memory_limit() noexcept
{
    constexpr ULONGLONG kCap = ULONGLONG{12} * 1024 * 1024 * 1024;
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    const ULONGLONG half = GlobalMemoryStatusEx(&ms) != 0 ? ms.ullTotalPageFile / 2 : kCap;
    return static_cast<SIZE_T>(std::min(kCap, half));
}

void drop_older(Engine& e, uint32_t id)
{
    std::vector<uint32_t> old;
    for (const auto& [k, it] : e.items) {
        if (k < id)
            old.push_back(k);
    }
    for (const uint32_t k : old)
        discard_item(e, k);
}

}

void close_cur(Engine& e)
{
    e.sh->set_playing(WinRtPlayer{});
    e.cur.player.close();
    e.trash.discard(e.cur.wav);
    e.cur = Playing{};
}

void try_play(Engine& e)
{
    if (!e.want)
        return;
    const auto it = e.items.find(e.want->id);
    if (it == e.items.end() || !it->second.ready)
        return;
    const ReqPtr req = std::move(e.want);
    e.want.reset();
    Playing p;
    p.wav = it->second.wav;
    e.items.erase(it);
    drop_older(e, req->id);
    if (!e.sh->current(req->gen)) {
        e.trash.discard(p.wav);
        return;
    }
    close_cur(e);
    if (!p.player.open_file(p.wav) || !proc::play_wav(e.sh, p.player, req->volume, req->rate, req->gen, req->id)) {
        p.player.close();
        e.trash.discard(p.wav);
        e.sh->post(req->gen, req->id, TtsEvent::Error, kTtsPlayFailed);
        return;
    }
    e.cur = std::move(p);
    e.sh->set_playing(e.cur.player);
    schedule(e);
}

void do_speak(Engine& e, const ReqPtr& req)
{
    if (!e.sh->current(req->gen))
        return;
    close_cur(e);
    drop_older(e, req->id);
    if (!add_item(e, req, e.fresh)) {
        e.sh->post(req->gen, req->id, TtsEvent::Error, kTtsSynthFailed);
        return;
    }
    e.fresh = false;
    e.want = req;
    try_play(e);
    schedule(e);
}

void do_prepare(Engine& e, const ReqPtr& req)
{
    if (!e.sh->epoch_is(req->epoch))
        return;
    if (add_item(e, req, false))
        schedule(e);
}

void do_stop(Engine& e)
{
    close_cur(e);
    abort_batch(e);
    std::vector<uint32_t> all;
    all.reserve(e.items.size());
    for (const auto& [id, it] : e.items)
        all.push_back(id);
    for (const uint32_t id : all)
        discard_item(e, id);
    e.want.reset();
    e.start_at.reset();
    e.fresh = true;
    e.trash.retry();
}

}

using vieneu::engine::Engine;
using vieneu::engine::Queue;
using vieneu::engine::Req;
using vieneu::engine::Shared;

struct VieNeuTts::Impl {
    piper::Cfg cfg;
    vieneu::Pins pins;
    std::shared_ptr<Shared> sh = std::make_shared<Shared>();
    std::shared_ptr<Queue> queue = std::make_shared<Queue>();
    std::shared_ptr<vieneu::engine::Verdict> verdict = std::make_shared<vieneu::engine::Verdict>();
    std::thread th;
    std::thread verify_th;

    void verify() noexcept
    {
        try {
            if (auto v = vieneu::verify_install(cfg, pins, vieneu::cache_file_of(cfg))) {
                const std::scoped_lock lk(verdict->m);
                verdict->v = std::make_shared<const vieneu::Verified>(std::move(*v));
            }
        } catch (...) { // NOLINT(bugprone-empty-catch): the engine just stays hidden
        }
        verdict->done = true;
    }

    void run() noexcept
    {
        {
            const WinRtApartment apartment;
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
        e.queue = queue;
        e.verdict = verdict;
        e.job = proc::make_job(vieneu::engine::kMaxProcs, vieneu::engine::job_memory_limit());
        if (e.job.valid() && piper::prepare_temp_dir(cfg.temp) && vieneu::prepare_play_dir(vieneu::play_dir(cfg)))
            loop(e);
        vieneu::engine::do_stop(e);
    }

    // Returns false when the engine must stop.
    bool take_task(Engine& e, Queue::Task& t)
    {
        std::unique_lock lk(queue->m);
        const auto ready = [&] { return queue->quit || !queue->q.empty(); };
        if (!ready() && !e.batch) {
            if (e.start_at)
                queue->cv.wait_until(lk, *e.start_at, ready);
            else
                queue->cv.wait(lk, ready);
        }
        if (queue->quit)
            return false;
        if (!queue->q.empty()) {
            t = std::move(queue->q.front());
            queue->q.pop_front();
        }
        return true;
    }

    void loop(Engine& e) noexcept
    {
        for (;;) {
            Queue::Task t;
            if (!take_task(e, t))
                return;
            try {
                if (t)
                    t(e);
                else if (e.batch)
                    vieneu::engine::pump(e);
                else if (e.start_at && vieneu::engine::Clock::now() >= *e.start_at)
                    vieneu::engine::maybe_start(e);
            } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
            }
        }
    }
};

VieNeuTts::VieNeuTts(const fs::path& libs_dir, const fs::path& data_dir, const fs::path& temp_dir)
    : VieNeuTts(libs_dir, data_dir, temp_dir, vieneu::builtin_pins())
{
}

VieNeuTts::VieNeuTts(const fs::path& libs_dir, const fs::path& data_dir, const fs::path& temp_dir, const vieneu::Pins& pins)
    : impl_(std::make_unique<Impl>())
{
    impl_->cfg = vieneu::make_cfg(libs_dir, data_dir, temp_dir);
    impl_->pins = pins;
    impl_->verify_th = std::thread([p = impl_.get()] { p->verify(); });
    impl_->th = std::thread([p = impl_.get()] { p->run(); });
}

VieNeuTts::~VieNeuTts()
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
    if (impl_->verify_th.joinable())
        impl_->verify_th.join();
    const std::scoped_lock lk(impl_->sh->m);
    impl_->sh->hwnd = nullptr;
}

std::vector<Voice> VieNeuTts::voices() noexcept
{
    std::vector<Voice> out;
    try {
        const auto v = impl_->verdict->get();
        if (!v)
            return out;
        for (const auto& p : v->presets) {
            out.push_back({p.id, p.name, L"vi-VN"});
            out.push_back({p.id, p.name, L"en-US"});
        }
    } catch (...) {
        out.clear();
    }
    return out;
}

bool VieNeuTts::speak(uint32_t id, std::string_view utf8, const std::wstring& voice_id, double rate, double volume) noexcept
{
    try {
        const uint32_t gen = impl_->sh->bump_gen();
        const bool too_big = utf8.size() > kTtsMaxUtf8;
        if (impl_->sh->dead || too_big || impl_->sh->voice_dead(voice_id)) {
            impl_->sh->post(gen, id, TtsEvent::Error, too_big ? kTtsTextTooLarge : kTtsSynthFailed);
            return false;
        }
        auto req = std::make_shared<const Req>(Req{gen, 0, id, std::string(utf8), voice_id, rate, volume});
        impl_->queue->push([req](Engine& e) { vieneu::engine::do_speak(e, req); });
        return true;
    } catch (...) {
        return false;
    }
}

void VieNeuTts::prepare(uint32_t id, std::string_view utf8, const std::wstring& voice_id, double rate) noexcept
{
    try {
        if (utf8.size() > kTtsMaxUtf8 || impl_->sh->dead || impl_->sh->voice_dead(voice_id))
            return;
        auto req = std::make_shared<const Req>(Req{0, impl_->sh->epoch_now(), id, std::string(utf8), voice_id, rate, 0.0});
        impl_->queue->push([req](Engine& e) { vieneu::engine::do_prepare(e, req); });
    } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
    }
}

void VieNeuTts::pause() noexcept
{
    impl_->sh->with_playing(false);
}

void VieNeuTts::resume() noexcept
{
    impl_->sh->with_playing(true);
}

void VieNeuTts::stop() noexcept
{
    impl_->sh->invalidate_all();
    impl_->queue->push([](Engine& e) { vieneu::engine::do_stop(e); });
}

void VieNeuTts::set_sink(HWND hwnd, UINT msg) noexcept
{
    const std::scoped_lock lk(impl_->sh->m);
    impl_->sh->hwnd = hwnd;
    impl_->sh->msg = msg;
}

bool VieNeuTts::ready() const noexcept
{
    return impl_->verdict->get() != nullptr;
}

size_t VieNeuTts::lookahead() const noexcept
{
    return vieneu::engine::kLookahead;
}

}
