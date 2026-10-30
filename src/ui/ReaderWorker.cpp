#include "ReaderWorker.h"

#include "ReaderSpread.h"
#include "base/FileIo.h"
#include "base/Log.h"
#include "doc/TxtToFb2.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <new>

namespace eb {
namespace {

constexpr int kMaxAttempts = 2;
constexpr int32_t kMaxTocItems = 20000;
constexpr std::size_t kMaxChapterBytes = 200;
constexpr std::size_t kMaxQueuedResults = 6;
constexpr long long kIdleAfterRequestMs = 400;

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
    const auto raw = read_all_bytes(job.path, kMaxTxtBytes);
    if (!raw)
        return raw.error().code == ErrorCode::Limit ? EB_E_LIMIT : EB_E_OPEN;
    const auto fb2 = txt_to_fb2(*raw, job.path.stem().wstring());
    if (!fb2)
        return fb2.error().code == ErrorCode::Limit ? EB_E_LIMIT : EB_E_FORMAT;
    return eb_open_memory(fb2->data(), fb2->size(), "fb2", doc);
}

struct TextFree {
    void operator()(eb_text* t) const noexcept { eb_text_free(t); }
};

struct LinksFree {
    void operator()(eb_link* l) const noexcept { eb_links_free(l); }
};

std::shared_ptr<const PageText> copy_text(const eb_text& t, int32_t page)
{
    auto pt = std::make_shared<PageText>();
    pt->page = page;
    const auto count = static_cast<std::size_t>(std::max(t.char_count, 0));
    const std::size_t n = std::min(count, kMaxPageChars);
    pt->utf8.assign(t.utf8, n < count ? t.utf8_offset[n] : t.utf8_len);
    pt->offset.assign(t.utf8_offset, t.utf8_offset + n);
    pt->box.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
        pt->box.push_back({t.box[i].x0, t.box[i].y0, t.box[i].x1, t.box[i].y1});
    return pt;
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
    queue_.supersede();
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

void RenderWorker::request_text(int32_t page, uint32_t numbering)
{
    queue_.post_text(page, numbering);
}

std::vector<RenderResult> RenderWorker::take()
{
    notified_.store(false);
    const std::lock_guard lock(out_mu_);
    return std::exchange(out_, {});
}

std::vector<TocItem> RenderWorker::toc() const
{
    const std::lock_guard lock(toc_mu_);
    return toc_;
}

void RenderWorker::post(RenderResult r)
{
    {
        const std::lock_guard lock(out_mu_);
        // Only page bitmaps are dropped: the read-aloud controller waits for every text result it asked for. The others stay
        // bounded: text requests are a FIFO capped at 4 and links come once per render.
        const auto bitmap = [](const RenderResult& o) { return o.kind == RenderResult::Kind::Page; };
        if (r.kind == RenderResult::Kind::Page && std::ranges::count_if(out_, bitmap) >= kMaxQueuedResults) {
            // Drop order: older generations, prefetched pages, the right page of the spread, the shown page.
            const auto drop = [&](auto pred) { return std::ranges::find_if(out_, pred); };
            auto victim = drop([&](const RenderResult& o) { return bitmap(o) && o.generation < r.generation; });
            if (victim == out_.end())
                victim = drop([&](const RenderResult& o) { return bitmap(o) && !o.primary && !o.companion; });
            if (victim == out_.end())
                victim = drop([&](const RenderResult& o) { return bitmap(o) && !o.primary; });
            out_.erase(victim != out_.end() ? victim : drop(bitmap));
        }
        out_.push_back(std::move(r));
    }
    if (!notified_.exchange(true) && !PostMessageW(notify_, msg_, 0, 0))
        notified_.store(false);
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

namespace {

bool is_open_failure(eb_status st) noexcept
{
    return st == EB_E_OPEN || st == EB_E_FORMAT || st == EB_E_DRM;
}

}

void RenderWorker::run(const std::stop_token& st, const OpenJob& job) noexcept
{
    stop_ = st;
    try {
        guard_.emplace(job.marker);
        scope_.emplace(*guard_, "open", job.id);
        const eb_status open_st = open_doc(job);
        if (!is_open_failure(open_st)) {
            RenderQueue::Job next;
            while (!st.stop_requested() && queue_.pop(next, st, idle_delay(open_st == EB_OK))) {
                after_idle_ = next.idle;
                if (next.idle)
                    run_idle();
                if (next.text_page && open_st == EB_OK)
                    serve_text(*next.text_page, next.text_numbering);
                if (next.render && open_st == EB_OK)
                    serve(*next.render);
                else if (next.render)
                    post_error(*next.render, open_st, 0);
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

eb_status RenderWorker::open_doc(const OpenJob& job)
{
    eb_doc* doc = nullptr;
    eb_status st = EB_E_INTERNAL;
    try {
        cache_ = job.cache;
        st = job.fmt == "txt" ? open_txt(job, &doc)
                              : eb_open_cached(job.path.c_str(), cache_.empty() ? nullptr : cache_.c_str(), &doc);
    } catch (const std::bad_alloc&) {
        st = EB_E_NOMEM;
    } catch (...) {
        st = EB_E_INTERNAL;
    }
    if (st == EB_OK) {
        {
            const std::lock_guard lock(doc_mu_);
            doc_ = doc;
        }
        reflowable_ = eb_is_reflowable(doc) != 0;
        if (!reflowable_) {
            st = eb_page_count(doc, &count_);
            if (st == EB_OK && count_ <= 0)
                st = EB_E_FORMAT;
            laid_out_ = st == EB_OK;
        }
    }
    if (is_open_failure(st))
        fail_open(st, eb_last_error());
    return st;
}

// Background layout steps wait a moment after the last request, and never run while one is queued.
std::chrono::milliseconds RenderWorker::idle_delay(bool open) const noexcept
{
    if (!open || complete_)
        return std::chrono::milliseconds(-1);
    return std::chrono::milliseconds(after_idle_ ? 0 : kIdleAfterRequestMs);
}

void RenderWorker::post_error(const RenderWant& w, eb_status st, int32_t page) noexcept
{
    try {
        log::error("reader: page {} failed: {}", page, eb_status_text(st));
        RenderResult r;
        r.generation = w.generation;
        r.view_id = w.view_id;
        r.page = page;
        r.page_count = count_;
        r.complete = complete_;
        r.primary = true;
        r.status = st;
        post(std::move(r));
    } catch (...) {
        log::error("reader: cannot report page failure");
    }
}

void RenderWorker::serve(const RenderWant& w)
{
    try {
        flush_numbering();
        for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
            if (dead_ || stop_.stop_requested())
                return;
            // A stray abort (issued when nothing was in flight) fails the next call once; retry unless superseded.
            if (serve_once(w) != EB_E_ABORTED || !queue_.current(w.generation))
                return;
        }
    } catch (const std::bad_alloc&) {
        post_error(w, EB_E_NOMEM, 0);
    } catch (...) {
        post_error(w, EB_E_INTERNAL, 0);
    }
}

void RenderWorker::serve_text(int32_t page, uint32_t numbering) noexcept
{
    RenderResult r;
    r.kind = RenderResult::Kind::Text;
    r.page = page;
    r.status = EB_E_NOMEM;
    try {
        eb_text* raw = nullptr;
        flush_numbering();
        if (numbering != numbering_)
            return;
        r.status = laid_out_ && page >= 0 && page < count_ ? EB_E_ABORTED : EB_E_RANGE;
        for (int again = 0; again < 3 && r.status == EB_E_ABORTED; ++again) {
            if (uint64_t mark = 0; eb_bookmark_of_page(doc_, page, &mark) != EB_OK)
                break;
            const Moved moved = numbering_moved();
            if (moved == Moved::Bumped)
                return;
            if (moved == Moved::None)
                break;
            if (page >= count_)
                r.status = EB_E_RANGE;
        }
        for (int attempt = 0; attempt < kMaxAttempts && r.status == EB_E_ABORTED; ++attempt)
            r.status = eb_page_text(doc_, page, &raw);
        const std::unique_ptr<eb_text, TextFree> t(raw);
        if (r.status == EB_OK && t)
            r.text = copy_text(*t, page);
    } catch (...) {
        log::error("reader: cannot read the text of page {}", page);
        r.status = EB_E_NOMEM;
        r.text.reset();
    }
    try {
        post(std::move(r));
    } catch (...) {
        log::error("reader: cannot report the text of page {}", page);
    }
}

eb_status RenderWorker::serve_once(const RenderWant& w)
{
    int32_t page = 0;
    if (w.numbering != numbering_)
        return EB_OK;
    eb_status st = ensure_layout(w, page);
    if (st != EB_OK && !laid_out_) {
        fail_open(st, eb_last_error());
        dead_ = true;
        return st;
    }
    if (st == EB_OK && primary_gen_ != w.generation) {
        st = deliver(w, page, !w.have_cur, true);
        if (st == EB_OK) {
            primary_gen_ = w.generation;
            scope_.reset();
            if (load_toc())
                deliver(w, page, false, true);
        }
    }
    if (st == EB_OK && companion_gen_ != w.generation) {
        st = deliver_companion(w, page);
        if (st == EB_OK)
            companion_gen_ = w.generation;
    }
    if (st != EB_OK) {
        if (st != EB_E_ABORTED)
            post_error(w, st, page);
        return st;
    }
    if (links_gen_ != w.generation) {
        st = serve_links(w, page);
        if (st != EB_OK)
            return st;
    }
    return prefetch(w, page);
}

eb_status RenderWorker::prefetch(const RenderWant& w, int32_t page)
{
    if (!w.prefetch)
        return EB_OK;
    const int32_t span = w.view.pages >= 2 ? 2 : 1;
    if (page + span < count_) {
        if (prefetch_spread(w, page + span, w.have_next, w.have_next2) == EB_E_ABORTED)
            return EB_E_ABORTED;
    }
    if (page > 0)
        return prefetch_spread(w, std::max(page - span, 0), w.have_prev, w.have_prev2) == EB_E_ABORTED ? EB_E_ABORTED : EB_OK;
    return EB_OK;
}

eb_status RenderWorker::prefetch_spread(const RenderWant& w, int32_t first, bool have_first, bool have_second)
{
    if (!have_first && deliver(w, first, true, false) == EB_E_ABORTED)
        return EB_E_ABORTED;
    if (has_right_page(first, count_, w.view.pages) && !have_second)
        return deliver(w, first + 1, true, false) == EB_E_ABORTED ? EB_E_ABORTED : EB_OK;
    return EB_OK;
}

eb_status RenderWorker::deliver_companion(const RenderWant& w, int32_t page)
{
    if (!has_right_page(page, count_, w.view.pages))
        return EB_OK;
    return deliver(w, page + 1, !w.have_cur2, false, true);
}

void RenderWorker::log_opts(const eb_render_opts& o)
{
    const std::array<uint32_t, 3> now{static_cast<uint32_t>(o.tint), o.fg_rgb, o.bg_rgb};
    if (now == logged_opts_)
        return;
    logged_opts_ = now;
    log::info("reader: render-opts tint={} fg={} bg={}", o.tint, format_hex_rgb(o.fg_rgb), format_hex_rgb(o.bg_rgb));
}

eb_status RenderWorker::page_scale(const RenderWant& w, int32_t page, float& scale)
{
    scale = render_scale(w.view.dpi);
    float pw = applied_.width_pt, ph = applied_.height_pt;
    if (!reflowable_) {
        if (const eb_status st = eb_page_size(doc_, page, &pw, &ph); st != EB_OK)
            return st;
        scale = fixed_scale(w.view.zoom, pw, ph, slot_width_px(w.view.client_w, w.view.dpi, w.view.pages), w.view.client_h,
            w.view.dpi, w.max_side_px);
    }
    scale = limit_scale(scale, pw, ph, w.max_side_px);
    return EB_OK;
}

eb_status RenderWorker::render_bitmap(const RenderWant& w, int32_t page, RenderResult& r)
{
    float scale = 0;
    if (const eb_status st = page_scale(w, page, scale); st != EB_OK)
        return st;
    const ReaderStyle& style = style_of(w);
    const eb_render_opts opts{scale, style.colors.fg, style.colors.bg, style.tint ? 1 : 0};
    log_opts(opts);
    eb_bitmap* b = nullptr;
    const eb_status st = eb_render_page(doc_, page, &opts, &b);
    r.bmp.reset(b);
    r.scale = scale;
    return st;
}

// Lays out the chapter of the page (and of the next page for the shown one) before anything is rendered. If that moves page
// numbers the view is told and asks again: EB_E_ABORTED.
eb_status RenderWorker::resolve_page(int32_t page, RenderResult& r)
{
    for (int again = 0; again < 3; ++again) {
        uint64_t mark = 0;
        if (const eb_status st = eb_bookmark_of_page(doc_, page, &mark); st != EB_OK)
            return st;
        if (r.primary) {
            r.mark = mark;
            r.chapter = chapter_of(mark);
            bail_mark_ = mark;
            if (page + 1 < count_ && eb_bookmark_of_page(doc_, page + 1, &r.end_mark) != EB_OK)
                r.end_mark = mark + 1;
        }
        const Moved moved = numbering_moved();
        if (moved == Moved::Bumped)
            return EB_E_ABORTED;
        if (moved == Moved::None)
            break;
    }
    bail_mark_.reset();
    r.page_count = count_;
    r.complete = complete_;
    return EB_OK;
}

eb_status RenderWorker::deliver(const RenderWant& w, int32_t page, bool with_bitmap, bool primary, bool companion)
{
    if (stop_.stop_requested())
        return EB_E_ABORTED;
    RenderResult r;
    r.generation = w.generation;
    r.view_id = w.view_id;
    r.page = page;
    r.primary = primary;
    r.companion = companion;
    r.reflowable = reflowable_;
    if (const eb_status st = resolve_page(page, r); st != EB_OK)
        return primary || st == EB_E_ABORTED ? st : EB_OK;
    const auto t = std::chrono::steady_clock::now();
    if (with_bitmap) {
        if (const eb_status st = render_bitmap(w, page, r); st != EB_OK)
            return primary ? st : (st == EB_E_ABORTED ? st : EB_OK);
    } else if (primary || companion) {
        if (float scale = 0; page_scale(w, page, scale) == EB_OK)
            r.scale = scale;
    }
    if (!queue_.current(w.generation))
        return EB_E_ABORTED;
    if (primary) {
        last_mark_ = r.mark;
        last_page_ = page;
        ref_valid_ = true;
        relayout_mark_.reset();
        if (with_bitmap && !first_logged_) {
            first_logged_ = true;
            log::info("reader: open-ms {} page-ms {}", ms_since(t0_), ms_since(t));
        }
    }
    post(std::move(r));
    return EB_OK;
}

eb_status RenderWorker::serve_links(const RenderWant& w, int32_t page)
{
    if (const eb_status st = post_links(w, page, false); st != EB_OK)
        return st;
    if (has_right_page(page, count_, w.view.pages)) {
        if (const eb_status st = post_links(w, page + 1, true); st != EB_OK)
            return st;
    }
    links_gen_ = w.generation;
    return EB_OK;
}

eb_status RenderWorker::post_links(const RenderWant& w, int32_t page, bool companion)
{
    RenderResult r;
    r.kind = RenderResult::Kind::Links;
    r.generation = w.generation;
    r.view_id = w.view_id;
    r.page = page;
    r.companion = companion;
    if (const eb_status st = load_links(page, r); st == EB_E_ABORTED)
        return st;
    if (!queue_.current(w.generation))
        return EB_E_ABORTED;
    post(std::move(r));
    return EB_OK;
}

eb_status RenderWorker::load_links(int32_t page, RenderResult& r)
{
    eb_link* raw = nullptr;
    int32_t n = 0;
    const eb_status st = eb_page_links(doc_, page, &raw, &n);
    const std::unique_ptr<eb_link, LinksFree> items(raw);
    if (st != EB_OK)
        return st;
    const auto count = std::min(static_cast<std::size_t>(std::max(n, 0)), kMaxPageLinks);
    r.links.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const eb_link& l = items.get()[i];
        r.links.push_back({{l.area.x0, l.area.y0, l.area.x1, l.area.y1}, l.target});
    }
    return EB_OK;
}

bool RenderWorker::load_toc()
{
    if (toc_loaded_)
        return false;
    toc_loaded_ = true;
    eb_toc_item* raw = nullptr;
    int32_t n = 0;
    if (eb_toc(doc_, &raw, &n) != EB_OK)
        return false;
    const auto free_toc = [n](eb_toc_item* p) { eb_toc_free(p, n); };
    const std::unique_ptr<eb_toc_item, decltype(free_toc)> items(raw, free_toc);
    std::vector<TocItem> list;
    list.reserve(static_cast<std::size_t>(std::min(n, kMaxTocItems)));
    for (int32_t i = 0; i < n && i < kMaxTocItems; ++i)
        list.push_back({items.get()[i].level, items.get()[i].target, trim_utf8(items.get()[i].title)});
    const std::lock_guard lock(toc_mu_);
    toc_ = std::move(list);
    return !toc_.empty();
}

std::string RenderWorker::chapter_of(uint64_t mark) const
{
    const TocItem* best = nullptr;
    for (const TocItem& e : toc_) {
        if (e.target <= mark && (best == nullptr || e.target >= best->target))
            best = &e;
    }
    return best != nullptr ? best->title : std::string{};
}

}
