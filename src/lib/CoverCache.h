#pragma once
#include "base/CrashGuard.h"
#include "base/Result.h"

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

struct eb_doc;
struct IWICImagingFactory;

namespace eb {

inline constexpr int kCoverSidePx = 320;
inline constexpr size_t kMaxPendingCovers = 64;
inline constexpr size_t kMaxReadyCovers = 8;

// Lets another thread abort the cover render that is running right now.
class AbortSlot {
public:
    void set(eb_doc* doc) noexcept;
    void abort() noexcept;

private:
    std::mutex mu_;
    eb_doc* doc_ = nullptr;
};

// data\covers\<id>.jpg, kCoverSidePx on the long side.
class CoverCache {
public:
    explicit CoverCache(std::filesystem::path dir) : dir_(std::move(dir)) {}

    [[nodiscard]] std::optional<std::filesystem::path> cached(std::string_view id) const noexcept;
    // Synchronous; needs eb_init and COM initialised (MTA) on the calling thread. wic may be null.
    Result<std::filesystem::path> generate(std::string_view id, const std::filesystem::path& book,
        IWICImagingFactory* wic = nullptr, AbortSlot* abort = nullptr, const std::stop_token& stop = {}) const noexcept;

private:
    [[nodiscard]] std::optional<std::filesystem::path> path_for(std::string_view id) const noexcept;

    std::filesystem::path dir_;
};

struct CoverResult {
    std::string id;
    bool ok = false;
    int width = 0, height = 0;
    std::vector<uint32_t> pixels; // premultiplied BGRA, width x height, the cover letterboxed on a neutral background
};

// One low-priority thread serving a LIFO queue of visible books. Results are drained on the caller's thread.
//
// Crash markers: a CrashLoopGuard has one "current operation" slot, so each thread that runs risky engine code
// owns its marker file (data\pending: startup/open path, data\pending_scan: scanner thread, data\pending_cover:
// this thread). If the process is killed while a cover is being made, that book id is read back as the suspect
// next start and blacklisted for good (kv cover.bad.<id>), even when the crash had another cause.
class CoverWorker {
public:
    // notify is called from the worker thread when results are waiting; it must be cheap and thread-safe, and returns
    // false when the signal could not be delivered (it is retried on the next request()).
    CoverWorker(CoverCache cache, std::filesystem::path marker_file, std::function<bool()> notify);
    CoverWorker(const CoverWorker&) = delete;
    CoverWorker& operator=(const CoverWorker&) = delete;
    ~CoverWorker() { stop(); }

    // The id whose cover generation was running when the previous session died; it is never retried.
    [[nodiscard]] const std::optional<std::string>& previous_suspect() const noexcept { return suspect_; }
    // cell_w x cell_h is the size of the finished cell. Returns the id evicted from a full queue, if any.
    std::optional<std::string> request(const std::string& id, const std::filesystem::path& book, int cell_w, int cell_h);
    std::vector<CoverResult> take_ready(size_t max);
    void stop() noexcept;

private:
    struct Job {
        std::string id;
        std::filesystem::path book;
        int w = 0, h = 0;
    };

    void run(const std::stop_token& stop) noexcept;
    void process(const Job& job, IWICImagingFactory* wic, CoverResult& r, const std::stop_token& stop) noexcept;
    void signal(bool call) noexcept;

    CoverCache cache_;
    CrashLoopGuard guard_;
    std::optional<std::string> suspect_;
    std::function<bool()> notify_;
    AbortSlot abort_;
    std::mutex mu_;
    std::condition_variable_any cv_;
    std::vector<Job> queue_;
    std::vector<CoverResult> ready_;
    bool notified_ = false;
    std::jthread thread_;
};

}
