#include "ReaderWorker.h"

#include "base/Log.h"
#include "doc/TxtToFb2.h"

#include <algorithm>
#include <cstddef>
#include <fstream>

namespace eb {
namespace {

constexpr int kMaxAttempts = 2;
constexpr int32_t kMaxTocItems = 20000;
constexpr std::size_t kMaxChapterBytes = 200;
constexpr std::size_t kMaxQueuedResults = 16;

std::string trim_utf8(const char* s)
{
    std::string out(s != nullptr ? s : "");
    if (out.size() > kMaxChapterBytes) {
        out.resize(kMaxChapterBytes);
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80)
            out.pop_back();
        if (!out.empty() && static_cast<unsigned char>(out.back()) >= 0xC0)
            out.pop_back();
    }
    return out;
}

eb_status open_txt(const OpenJob& job, eb_doc** doc)
{
    std::ifstream in(job.path, std::ios::binary | std::ios::ate);
    if (!in)
        return EB_E_OPEN;
    const auto size = in.tellg();
    if (size < 0 || static_cast<uint64_t>(size) > kMaxTxtBytes)
        return EB_E_LIMIT;
    std::vector<std::byte> raw(static_cast<std::size_t>(size));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(raw.data()), size))
        return EB_E_OPEN;
    const auto fb2 = txt_to_fb2(raw, job.path.stem().wstring());
    if (!fb2)
        return fb2.error().code == ErrorCode::Limit ? EB_E_LIMIT : EB_E_FORMAT;
    return eb_open_memory(fb2->data(), fb2->size(), "fb2", doc);
}

long long ms_since(std::chrono::steady_clock::time_point t) noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t).count();
}

}

RenderWorker::RenderWorker(HWND notify, UINT msg, OpenJob job)
    : notify_(notify), msg_(msg), queue_([this] {
          const std::lock_guard lock(doc_mu_);
          if (doc_ != nullptr)
              eb_abort(doc_);
      }),
      t0_(std::chrono::steady_clock::now()),
      thread_([this, j = std::move(job)](const std::stop_token& st) { run(st, j); })
{
}

RenderWorker::~RenderWorker()
{
    thread_.request_stop();
    abort_doc();
    thread_.join();
}

void RenderWorker::abort_doc() noexcept
{
    const std::lock_guard lock(doc_mu_);
    if (doc_ != nullptr)
        eb_abort(doc_);
}

uint64_t RenderWorker::request(const RenderWant& w)
{
    return queue_.post(w);
}

std::vector<RenderResult> RenderWorker::take()
{
    notified_.store(false);
    const std::lock_guard lock(out_mu_);
    return std::exchange(out_, {});
}

void RenderWorker::post(RenderResult r)
{
    {
        const std::lock_guard lock(out_mu_);
        if (out_.size() >= kMaxQueuedResults && !r.primary)
            return;
        out_.push_back(std::move(r));
    }
    if (!notified_.exchange(true))
        PostMessageW(notify_, msg_, 0, 0);
}

void RenderWorker::fail_open(eb_status st, std::string detail) noexcept
{
    try {
        RenderResult r;
        r.kind = RenderResult::Kind::OpenFailed;
        r.status = st;
        r.detail = std::move(detail);
        post(std::move(r));
    } catch (...) {
        log::error("reader: cannot report open failure");
    }
}

void RenderWorker::release_doc() noexcept
{
    eb_doc* d = nullptr;
    {
        const std::lock_guard lock(doc_mu_);
        d = std::exchange(doc_, nullptr);
    }
    if (d != nullptr)
        eb_close(d);
}

void RenderWorker::run(const std::stop_token& st, const OpenJob& job) noexcept
{
    try {
        guard_.emplace(job.marker);
        scope_.emplace(*guard_, "open", job.id);
        if (open_doc(job)) {
            RenderWant w;
            while (queue_.pop(w, st)) {
                serve(w);
                queue_.done();
            }
        }
    } catch (...) {
        log::error("reader: render worker failed");
        fail_open(EB_E_INTERNAL, "internal error");
    }
    scope_.reset();
    release_doc();
}

bool RenderWorker::open_doc(const OpenJob& job)
{
    eb_doc* doc = nullptr;
    const eb_status st = job.fmt == "txt" ? open_txt(job, &doc) : eb_open(job.path.c_str(), &doc);
    if (st != EB_OK) {
        fail_open(st, eb_last_error());
        return false;
    }
    {
        const std::lock_guard lock(doc_mu_);
        doc_ = doc;
    }
    reflowable_ = eb_is_reflowable(doc) != 0;
    if (!reflowable_) {
        if (const eb_status pc = eb_page_count(doc, &count_); pc != EB_OK || count_ <= 0) {
            fail_open(pc != EB_OK ? pc : EB_E_FORMAT, eb_last_error());
            return false;
        }
        laid_out_ = true;
    }
    return true;
}

void RenderWorker::serve(const RenderWant& w)
{
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        // A stray abort (issued when nothing was in flight) fails the next call once; retry unless superseded.
        if (serve_once(w) != EB_E_ABORTED || !queue_.current(w.generation))
            return;
    }
}

eb_status RenderWorker::apply_layout(const LayoutParams& lp)
{
    if (const eb_status st = eb_layout(doc_, lp.width_pt, lp.height_pt, lp.em_pt, nullptr); st != EB_OK)
        return st;
    if (const eb_status st = eb_page_count(doc_, &count_); st != EB_OK)
        return st;
    applied_ = lp;
    laid_out_ = true;
    return count_ > 0 ? EB_OK : EB_E_FORMAT;
}

eb_status RenderWorker::ensure_layout(const RenderWant& w, int32_t& page)
{
    uint64_t mark = w.anchor;
    if (reflowable_) {
        const LayoutParams lp = reflow_layout(w.view.client_w, w.view.client_h, w.view.dpi);
        if (!laid_out_ || !(lp == applied_)) {
            if (const eb_status st = apply_layout(lp); st != EB_OK)
                return st;
            if (mark == kNoAnchor && last_mark_)
                mark = *last_mark_;
        }
    }
    page = w.page;
    if (mark != kNoAnchor) {
        if (const eb_status st = eb_page_of_bookmark(doc_, mark, &page); st != EB_OK)
            return st;
    }
    page = std::clamp(page, 0, count_ - 1);
    return EB_OK;
}

eb_status RenderWorker::serve_once(const RenderWant& w)
{
    int32_t page = 0;
    eb_status st = ensure_layout(w, page);
    if (st == EB_OK)
        st = deliver(w, page, !w.have_cur, true);
    if (st != EB_OK) {
        if (st != EB_E_ABORTED) {
            log::error("reader: page {} failed: {}", page, eb_status_text(st));
            RenderResult r;
            r.generation = w.generation;
            r.view_id = w.view_id;
            r.page = page;
            r.page_count = count_;
            r.primary = true;
            r.status = st;
            post(std::move(r));
        }
        return st;
    }
    scope_.reset();
    if (load_toc())
        deliver(w, page, false, true);
    if (!w.have_next && page + 1 < count_) {
        st = deliver(w, page + 1, true, false);
        if (st == EB_E_ABORTED)
            return st;
    }
    if (!w.have_prev && page > 0)
        st = deliver(w, page - 1, true, false);
    return st == EB_E_ABORTED ? st : EB_OK;
}

eb_status RenderWorker::render_bitmap(const RenderWant& w, int32_t page, RenderResult& r)
{
    float scale = render_scale(w.view.dpi);
    if (!reflowable_) {
        float pw = 0, ph = 0;
        if (const eb_status st = eb_page_size(doc_, page, &pw, &ph); st != EB_OK)
            return st;
        scale = fixed_scale(w.view.zoom, pw, ph, w.view.client_w, w.view.client_h, w.view.dpi);
    }
    const eb_render_opts opts{scale, 0x000000, 0xFFFFFF, 0};
    eb_bitmap* b = nullptr;
    const eb_status st = eb_render_page(doc_, page, &opts, &b);
    r.bmp.reset(b);
    r.scale = scale;
    return st;
}

eb_status RenderWorker::deliver(const RenderWant& w, int32_t page, bool with_bitmap, bool primary)
{
    RenderResult r;
    r.generation = w.generation;
    r.view_id = w.view_id;
    r.page = page;
    r.page_count = count_;
    r.primary = primary;
    r.reflowable = reflowable_;
    if (primary) {
        if (const eb_status st = eb_bookmark_of_page(doc_, page, &r.mark); st != EB_OK)
            return st;
        r.chapter = chapter_of(r.mark);
    }
    const auto t = std::chrono::steady_clock::now();
    if (with_bitmap) {
        if (const eb_status st = render_bitmap(w, page, r); st != EB_OK)
            return primary ? st : (st == EB_E_ABORTED ? st : EB_OK);
    }
    if (!queue_.current(w.generation))
        return EB_E_ABORTED;
    if (primary) {
        last_mark_ = r.mark;
        if (with_bitmap && !first_logged_) {
            first_logged_ = true;
            log::info("reader: open-ms {} page-ms {}", ms_since(t0_), ms_since(t));
        }
    }
    post(std::move(r));
    return EB_OK;
}

bool RenderWorker::load_toc()
{
    if (toc_loaded_)
        return false;
    toc_loaded_ = true;
    eb_toc_item* items = nullptr;
    int32_t n = 0;
    if (eb_toc(doc_, &items, &n) != EB_OK)
        return false;
    toc_.reserve(static_cast<std::size_t>(std::min(n, kMaxTocItems)));
    for (int32_t i = 0; i < n && i < kMaxTocItems; ++i)
        toc_.push_back({items[i].target, trim_utf8(items[i].title)});
    eb_toc_free(items, n);
    return !toc_.empty();
}

std::string RenderWorker::chapter_of(uint64_t mark) const
{
    const TocEntry* best = nullptr;
    for (const TocEntry& e : toc_) {
        if (e.target <= mark && (best == nullptr || e.target >= best->target))
            best = &e;
    }
    return best != nullptr ? best->title : std::string{};
}

}
