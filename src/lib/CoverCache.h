#pragma once
#include "base/CrashGuard.h"
#include "base/Result.h"

#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace eb {

inline constexpr int kCoverSidePx = 320;
inline constexpr size_t kMaxPendingCovers = 64;

// data\covers\<id>.jpg, kCoverSidePx on the long side.
class CoverCache {
public:
    explicit CoverCache(std::filesystem::path dir) : dir_(std::move(dir)) {}

    [[nodiscard]] std::optional<std::filesystem::path> cached(std::string_view id) const noexcept;
    // Synchronous; needs eb_init. Initialises COM (MTA) on the calling thread for the duration.
    Result<std::filesystem::path> generate(std::string_view id, const std::filesystem::path& book) const noexcept;

private:
    [[nodiscard]] std::optional<std::filesystem::path> path_for(std::string_view id) const noexcept;

    std::filesystem::path dir_;
};

struct CoverResult {
    std::string id;
    bool ok = false;
};

// One low-priority thread serving a LIFO queue of visible books. Results are drained on the caller's thread.
class CoverWorker {
public:
    // notify is called from the worker thread when results are waiting; it must be cheap and thread-safe, and returns false when the signal could not be delivered.
    CoverWorker(CoverCache cache, std::filesystem::path marker_file, std::function<bool()> notify);
    CoverWorker(const CoverWorker&) = delete;
    CoverWorker& operator=(const CoverWorker&) = delete;
    ~CoverWorker() { stop(); }

    // The id whose cover generation was running when the previous session died; it is never retried.
    [[nodiscard]] const std::optional<std::string>& previous_suspect() const noexcept { return suspect_; }
    // Returns the id evicted from a full queue, if any.
    std::optional<std::string> request(const std::string& id, const std::filesystem::path& book);
    std::vector<CoverResult> take_ready(size_t max);
    void stop() noexcept;

private:
    struct Job {
        std::string id;
        std::filesystem::path book;
    };

    void run(const std::stop_token& stop) noexcept;
    bool process(const Job& job) noexcept;

    CoverCache cache_;
    CrashLoopGuard guard_;
    std::optional<std::string> suspect_;
    std::function<bool()> notify_;
    std::mutex mu_;
    std::condition_variable_any cv_;
    std::vector<Job> queue_;
    std::vector<CoverResult> ready_;
    bool notified_ = false;
    std::jthread thread_;
};

}
