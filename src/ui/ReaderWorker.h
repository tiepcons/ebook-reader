#pragma once
#include "ReaderLogic.h"
#include "base/CrashGuard.h"
#include "eb_mupdf.h"

#include <windows.h>

#include <array>
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
    enum class Kind : uint8_t { Page, OpenFailed, Text, Links };
    Kind kind = Kind::Page;
    uint64_t generation = 0;
    uint32_t view_id = 0;
    float scale = 0;        // device pixels per point of bmp
    int32_t page = 0, page_count = 0;
    uint64_t mark = 0;      // eb_bookmark_of_page(page); only set when primary
    uint64_t end_mark = UINT64_MAX; // bookmark of the next page, UINT64_MAX on the last one; only set when primary
    bool primary = false;   // the page the UI asked for, as opposed to a prefetch
    bool companion = false; // the right page of the spread whose left page is the primary one
    bool reflowable = false;
    eb_status status = EB_OK;
    BitmapPtr bmp;          // null when the UI already holds it or on failure (status != EB_OK)
    std::string chapter;    // UTF-8, primary only
    std::string detail;     // UTF-8, OpenFailed only
    std::vector<PageLink> links; // Links only: internal links of `page`, may be empty
    std::shared_ptr<const PageText> text; // Text only; null when the page has none or reading it failed
};

struct OpenJob {
    std::filesystem::path path, marker;
    std::string id, fmt;
    std::filesystem::path cache; // layout cache file; empty = none
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
    // The text of `page` in the current layout arrives as a Kind::Text result.
    void request_text(int32_t page);
    // Drains the results; re-arms the notification message.
    std::vector<RenderResult> take();
    // Outline of the book; empty until the first page is shown or when the book has none.
    std::vector<TocItem> toc() const;

private:
    void run(const std::stop_token& st, const OpenJob& job) noexcept;
    eb_status open_doc(const OpenJob& job);
    void serve(const RenderWant& w);
    void serve_text(int32_t page) noexcept;
    eb_status serve_once(const RenderWant& w);
    eb_status prefetch(const RenderWant& w, int32_t page);
    [[nodiscard]] uint64_t shown_mark(const RenderWant& w) const;
    void post_error(const RenderWant& w, eb_status st, int32_t page) noexcept;
    eb_status ensure_layout(const RenderWant& w, int32_t& page);
    eb_status apply_layout(const LayoutParams& lp, const std::string& css);
    void save_cache() noexcept;
    eb_status deliver(const RenderWant& w, int32_t page, bool with_bitmap, bool primary, bool companion = false);
    eb_status deliver_companion(const RenderWant& w, int32_t page);
    eb_status prefetch_spread(const RenderWant& w, int32_t first, bool have_first, bool have_second);
    void log_opts(const eb_render_opts& o);
    eb_status page_scale(const RenderWant& w, int32_t page, float& scale);
    eb_status render_bitmap(const RenderWant& w, int32_t page, RenderResult& r);
    bool load_toc();
    eb_status load_links(int32_t page, RenderResult& r);
    eb_status serve_links(const RenderWant& w, int32_t page);
    eb_status post_links(const RenderWant& w, int32_t page, bool companion);
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
    bool reflowable_ = false, laid_out_ = false, toc_loaded_ = false, first_logged_ = false, dead_ = false, cache_failed_ = false;
    int32_t count_ = 0;
    uint64_t primary_gen_ = 0, resolved_gen_ = 0, links_gen_ = 0, companion_gen_ = 0;
    int32_t resolved_page_ = 0, last_page_ = -1;
    LayoutParams applied_;
    std::string applied_css_;
    std::array<uint32_t, 3> logged_opts_{UINT32_MAX, UINT32_MAX, UINT32_MAX};
    std::optional<uint64_t> last_mark_;
    mutable std::mutex toc_mu_;
    std::vector<TocItem> toc_;
    std::filesystem::path cache_;
    std::optional<std::pair<LayoutParams, std::string>> saved_; // layout the cache file holds

    std::jthread thread_;
};

}
