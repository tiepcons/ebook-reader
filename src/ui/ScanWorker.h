#pragma once
#include "UiMessages.h"
#include "lib/Scanner.h"

#include <atomic>
#include <filesystem>
#include <thread>
#include <vector>

namespace eb {

struct ScanJob {
    std::filesystem::path db_file, marker_file;
    std::vector<std::filesystem::path> roots;
    std::vector<std::wstring> exclude_dirs;
    MetaFn meta;
};

// Title = file stem, author empty, fmt = lower-case extension. Replaced by the engine metadata in T2.3.
Result<BookMeta> stem_meta(const std::filesystem::path& file);

// UI-thread owner of the single background scan. The worker opens its own connection and reports
// through WM_APP_SCAN_PROGRESS and WM_APP_SCAN_DONE; the owner must call finish() when it receives DONE.
class ScanWorker {
public:
    ScanWorker() = default;
    ScanWorker(const ScanWorker&) = delete;
    ScanWorker& operator=(const ScanWorker&) = delete;
    ~ScanWorker() { stop(); }

    [[nodiscard]] bool running() const noexcept { return running_; }
    bool start(HWND notify, ScanJob job) noexcept;
    void finish() noexcept;
    void stop() noexcept;

private:
    static void run(HWND notify, const ScanJob& job, const std::atomic<bool>& cancel) noexcept;

    std::atomic<bool> cancel_{false};
    bool running_ = false;
    std::jthread thread_;
};

}
