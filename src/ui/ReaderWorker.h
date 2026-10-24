#pragma once
#include "ReaderLogic.h"
#include "base/CrashGuard.h"
#include "eb_mupdf.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace eb {

struct BitmapFree {
    void operator()(eb_bitmap* b) const noexcept { eb_bitmap_free(b); }
};
using BitmapPtr = std::unique_ptr<eb_bitmap, BitmapFree>;

struct RenderResult {
    enum class Kind : uint8_t { Page, OpenFailed };
    Kind kind = Kind::Page;
    uint64_t generation = 0;
    uint32_t view_id = 0;
    float scale = 0;        // device pixels per point of bmp
    int32_t page = 0, page_count = 0;
    uint64_t mark = 0;      // eb_bookmark_of_page(page); only set when primary
    bool primary = false;   // the page the UI asked for, as opposed to a prefetch
    bool reflowable = false;
    eb_status status = EB_OK;
    BitmapPtr bmp;          // null when the UI already holds it or on failure (status != EB_OK)
    std::string chapter;    // UTF-8, primary only
    std::string detail;     // UTF-8, OpenFailed only
};

struct OpenJob {
    std::filesystem::path path, marker;
    std::string id, fmt;
};

// One thread owns the eb_doc for its whole life; the UI thread only posts requests and takes results.
// Destruction aborts a running render, but eb_open and eb_layout cannot be aborted: closing waits for them.
class RenderWorker {
public:
    RenderWorker(HWND notify, UINT msg, OpenJob job);
    RenderWorker(const RenderWorker&) = delete;
    RenderWorker& operator=(const RenderWorker&) = delete;
    ~RenderWorker();

    uint64_t request(const RenderWant& w);
    // Drains the results; re-arms the notification message.
    std::vector<RenderResult> take();

private:
    struct TocEntry {
        uint64_t target;
        std::string title;
    };

    void run(const std::stop_token& st, const OpenJob& job) noexcept;
    eb_status open_doc(const OpenJob& job);
    void serve(const RenderWant& w);
    eb_status serve_once(const RenderWant& w);
    eb_status prefetch(const RenderWant& w, int32_t page);
    [[nodiscard]] uint64_t shown_mark(const RenderWant& w) const;
    void post_error(const RenderWant& w, eb_status st, int32_t page) noexcept;
    eb_status ensure_layout(const RenderWant& w, int32_t& page);
    eb_status apply_layout(const LayoutParams& lp);
    eb_status deliver(const RenderWant& w, int32_t page, bool with_bitmap, bool primary);
    eb_status render_bitmap(const RenderWant& w, int32_t page, RenderResult& r);
    bool load_toc();
    [[nodiscard]] std::string chapter_of(uint64_t mark) const;
    void post(RenderResult r);
    void fail_open(eb_status st, std::string detail) noexcept;
    void release_doc() noexcept;
    void abort_doc() noexcept;

    HWND notify_;
    UINT msg_;
    RenderQueue queue_;
    std::mutex doc_mu_;
    eb_doc* doc_ = nullptr;
    std::mutex out_mu_;
    std::vector<RenderResult> out_;
    std::atomic<bool> notified_{false};

    std::optional<CrashLoopGuard> guard_;
    std::optional<CrashLoopGuard::Scope> scope_;
    std::chrono::steady_clock::time_point t0_;
    std::stop_token stop_;
    bool reflowable_ = false, laid_out_ = false, toc_loaded_ = false, first_logged_ = false, dead_ = false;
    int32_t count_ = 0;
    uint64_t primary_gen_ = 0, resolved_gen_ = 0;
    int32_t resolved_page_ = 0, last_page_ = -1;
    LayoutParams applied_;
    std::optional<uint64_t> last_mark_;
    std::vector<TocEntry> toc_;

    std::jthread thread_;
};

}
