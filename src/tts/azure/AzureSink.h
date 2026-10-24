#pragma once
#include "tts/TtsEngine.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace eb {

// Event window plus the generation counters that gate every posted event.
// gen changes on speak() and stop(); epoch changes on stop() only (it guards prepare()).
class AzureSink {
public:
    void set(HWND hwnd, UINT msg) noexcept
    {
        const std::scoped_lock lk(m_);
        hwnd_ = hwnd;
        msg_ = msg;
    }
    void clear() noexcept { set(nullptr, 0); }

    uint32_t begin_speak() noexcept
    {
        const std::scoped_lock lk(m_);
        ++gen_;
        cv_.notify_all();
        return gen_;
    }
    void invalidate() noexcept
    {
        const std::scoped_lock lk(m_);
        ++gen_;
        ++epoch_;
        cv_.notify_all();
    }
    uint32_t epoch() noexcept
    {
        const std::scoped_lock lk(m_);
        return epoch_;
    }
    bool current(uint32_t gen) noexcept
    {
        const std::scoped_lock lk(m_);
        return gen == gen_;
    }
    bool valid(bool speak, uint32_t gen, uint32_t epoch) noexcept
    {
        const std::scoped_lock lk(m_);
        return speak ? gen == gen_ : epoch == epoch_;
    }
    // False when the request became stale while waiting.
    bool sleep_while_valid(std::chrono::milliseconds d, bool speak, uint32_t gen, uint32_t epoch) noexcept
    {
        std::unique_lock lk(m_);
        return !cv_.wait_for(lk, d, [&] { return speak ? gen != gen_ : epoch != epoch_; });
    }
    void post(uint32_t gen, uint32_t id, TtsEvent ev, uint32_t value) noexcept
    {
        const std::scoped_lock lk(m_);
        if (gen == gen_ && hwnd_ != nullptr)
            PostMessageW(hwnd_, msg_, id, pack_tts(ev, value));
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    HWND hwnd_ = nullptr;
    UINT msg_ = 0;
    uint32_t gen_ = 0;
    uint32_t epoch_ = 0;
};

// One worker thread running queued tasks in order; pending tasks are dropped on destruction.
class AzureTaskThread {
public:
    using Task = std::function<void()>;

    explicit AzureTaskThread(Task on_start = {}, Task on_exit = {})
        : th_([this, s = std::move(on_start), e = std::move(on_exit)] { run(s, e); })
    {
    }
    AzureTaskThread(const AzureTaskThread&) = delete;
    AzureTaskThread& operator=(const AzureTaskThread&) = delete;
    ~AzureTaskThread()
    {
        {
            const std::scoped_lock lk(m_);
            quit_ = true;
        }
        cv_.notify_one();
        if (th_.joinable())
            th_.join();
    }

    void push(Task t) noexcept
    {
        try {
            {
                const std::scoped_lock lk(m_);
                q_.push_back(std::move(t));
            }
            cv_.notify_one();
        } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
        }
    }

private:
    void run(const Task& on_start, const Task& on_exit) noexcept
    {
        try {
            if (on_start)
                on_start();
        } catch (...) { // NOLINT(bugprone-empty-catch): tasks fail individually
        }
        for (;;) {
            Task t;
            {
                std::unique_lock lk(m_);
                cv_.wait(lk, [&] { return quit_ || !q_.empty(); });
                if (quit_)
                    break;
                t = std::move(q_.front());
                q_.pop_front();
            }
            try {
                t();
            } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
            }
        }
        try {
            if (on_exit)
                on_exit();
        } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
        }
    }

    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Task> q_;
    bool quit_ = false;
    std::thread th_;
};

}
