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

inline long long ms_since(std::chrono::steady_clock::time_point t) noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t).count();
}

inline const ReaderStyle& style_of(const RenderWant& w) noexcept
{
    static const ReaderStyle kDefault;
    return w.style ? *w.style : kDefault;
}

struct RenderResult {
    enum class Kind : uint8_t { Page, OpenFailed, Text, Links, Layout };
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
    // Layout only. The book is laid out in the background, so page numbers and the page count are provisional until `complete`.
    // A result with a new `numbering` says that the page numbers of the pages already delivered moved by `shift` (`page` is the
    // new number of the page delivered last, `page_count` the new count); requests carry the numbering they were made in and the
    // worker drops those of an older one, so the view repeats them after it has applied the shift.
    uint32_t numbering = 0;
    int32_t shift = 0;
    uint64_t requested_mark = UINT64_MAX; // bookmark of the page whose request was cut short by the renumbering, if any
    bool complete = false;
    bool toc_changed = false; // the outline was read again (its targets are exact now): fetch it with toc()
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
    // The text of `page` (in the given numbering) arrives as a Kind::Text result.
    void request_text(int32_t page, uint32_t numbering);
    // Drains the results; re-arms the notification message.
    std::vector<RenderResult> take();
    // Outline of the book; empty until the first page is shown or when the book has none.
    std::vector<TocItem> toc() const;

private:
    void run(const std::stop_token& st, const OpenJob& job) noexcept;
    eb_status open_doc(const OpenJob& job);
    void serve(const RenderWant& w);
    void serve_text(int32_t page, uint32_t numbering) noexcept;
    eb_status serve_once(const RenderWant& w);
    eb_status prefetch(const RenderWant& w, int32_t page);
    [[nodiscard]] uint64_t shown_mark(const RenderWant& w) const;
    void post_error(const RenderWant& w, eb_status st, int32_t page) noexcept;
    eb_status ensure_layout(const RenderWant& w, int32_t& page);
    eb_status apply_layout(const LayoutParams& lp, const std::string& css);
    void save_cache() noexcept;
    void run_idle() noexcept;
    [[nodiscard]] std::chrono::milliseconds idle_delay(bool open) const noexcept;
    void finish_layout();
    // What a call that may have laid out a chapter did to the page numbers: Quiet = only the page count (or numbers the view does
    // not hold) changed, so the caller recomputes its own page; Bumped = numbers the view holds moved, a Layout result with a new
    // numbering was posted and requests made before it are stale.
    enum class Moved : uint8_t { None, Quiet, Bumped };
    Moved numbering_moved();
    void note_generation();
    // Tells the view about renumbering that background steps left unannounced.
    Moved flush_numbering();
    bool publish_layout(bool check_shift, bool toc_changed);
    eb_status resolve_page(int32_t page, RenderResult& r);
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
    std::chrono::steady_clock::time_point t0_, layout_t0_, published_;
    std::stop_token stop_;
    uint64_t primary_gen_ = 0, resolved_gen_ = 0, links_gen_ = 0, companion_gen_ = 0;
    std::optional<uint64_t> last_mark_;
    std::optional<uint64_t> relayout_mark_; // position to restore while the first page of a new layout is not delivered yet
    std::optional<uint64_t> bail_mark_;     // bookmark of the shown page being resolved when the numbering moved
    int32_t count_ = 0;
    int32_t resolved_page_ = 0, last_page_ = -1;
    uint32_t layout_gen_ = 0, numbering_ = 0;
    int steps_ = 0;
    LayoutParams applied_;
    std::string applied_css_;
    std::array<uint32_t, 3> logged_opts_{UINT32_MAX, UINT32_MAX, UINT32_MAX};
    bool reflowable_ = false, laid_out_ = false, toc_loaded_ = false, first_logged_ = false, dead_ = false, cache_failed_ = false;
    bool complete_ = true, ref_valid_ = false, numbering_dirty_ = false, after_idle_ = false;
    mutable std::mutex toc_mu_;
    std::vector<TocItem> toc_;
    std::filesystem::path cache_;
    std::optional<std::pair<LayoutParams, std::string>> saved_; // layout the cache file holds

    std::jthread thread_;
};

}
