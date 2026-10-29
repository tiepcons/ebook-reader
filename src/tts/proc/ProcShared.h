#pragma once
#include "base/Handle.h"
#include "tts/TtsEngine.h"
#include "tts/winrt/WinRtPlayer.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// State and helpers shared by the engines that run an external synthesizer as a sandboxed child process (Piper, VieNeu).
namespace eb::proc {

// What the UI thread and the worker thread both touch. `gen` guards utterances started by speak(); `epoch` guards
// prepared ones; both move on stop().
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
    WinRtPlayer playing;

    bool current(uint32_t g) noexcept;
    bool epoch_is(uint32_t e) noexcept;
    uint32_t epoch_now() noexcept;
    uint32_t bump_gen() noexcept;
    void invalidate_all() noexcept;
    void post(uint32_t g, uint32_t id, TtsEvent ev, uint32_t value) noexcept;
    bool voice_dead(const std::wstring& v) noexcept;
    void note(const std::wstring& v, bool ok) noexcept;
    void set_playing(WinRtPlayer p) noexcept;
    void with_playing(bool play) noexcept;
};

template <class Engine>
struct Queue {
    using Task = std::function<void(Engine&)>;

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
    [[nodiscard]] bool has_work() noexcept
    {
        const std::scoped_lock lk(m);
        return quit || !q.empty();
    }
};

// Kill-on-close job that holds the children: at most `max_procs` processes and `memory` committed bytes.
UniqueHandle make_job(DWORD max_procs, SIZE_T memory) noexcept;

// Files that could not be deleted yet (still open elsewhere) are retried on the next call and at shutdown.
struct Trash {
    std::vector<std::filesystem::path> pending;

    void discard(const std::filesystem::path& p) noexcept;
    void retry() noexcept;
};

// Starts `player` and forwards its events as TtsEvents of utterance `id`, dropped when `gen` is no longer current.
bool play_wav(const std::shared_ptr<Shared>& sh, WinRtPlayer& player, double volume, double rate, uint32_t gen,
    uint32_t id) noexcept;

}
