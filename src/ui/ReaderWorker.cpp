#include "ReaderWorker.h"

#include "base/Log.h"
#include "doc/TxtToFb2.h"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <memory>
#include <new>

namespace eb {
namespace {

constexpr int kMaxAttempts = 2;
constexpr int32_t kMaxTocItems = 20000;
constexpr std::size_t kMaxChapterBytes = 200;
constexpr std::size_t kMaxQueuedResults = 4;

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

const ReaderStyle& style_of(const RenderWant& w) noexcept
{
    static const ReaderStyle kDefault;
    return w.style ? *w.style : kDefault;
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

void RenderWorker::request_text(int32_t page)
{
    queue_.post_text(page);
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
        if (out_.size() >= kMaxQueuedResults) {
            const auto stale = std::ranges::find_if(out_, [&](const RenderResult& o) { return o.generation < r.generation; });
            const auto spare = std::ranges::find_if(out_, [](const RenderResult& o) { return !o.primary; });
            out_.erase(stale != out_.end() ? stale : (spare != out_.end() ? spare : out_.begin()));
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
            while (queue_.pop(next, st)) {
                if (next.text_page && open_st == EB_OK)
                    serve_text(*next.text_page);
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
        st = job.fmt == "txt" ? open_txt(job, &doc) : eb_open(job.path.c_str(), &doc);
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

void RenderWorker::post_error(const RenderWant& w, eb_status st, int32_t page) noexcept
{
    try {
        log::error("reader: page {} failed: {}", page, eb_status_text(st));
        RenderResult r;
        r.generation = w.generation;
        r.view_id = w.view_id;
        r.page = page;
        r.page_count = count_;
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

void RenderWorker::serve_text(int32_t page) noexcept
{
    RenderResult r;
    r.kind = RenderResult::Kind::Text;
    r.page = page;
    r.status = EB_E_NOMEM;
    try {
        eb_text* raw = nullptr;
        r.status = laid_out_ && page >= 0 && page < count_ ? EB_E_ABORTED : EB_E_RANGE;
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

eb_status RenderWorker::apply_layout(const LayoutParams& lp, const std::string& css)
{
    if (const eb_status st = eb_layout(doc_, lp.width_pt, lp.height_pt, lp.em_pt, css.empty() ? nullptr : css.c_str());
        st != EB_OK)
        return st;
    if (const eb_status st = eb_page_count(doc_, &count_); st != EB_OK)
        return st;
    if (count_ <= 0)
        return EB_E_FORMAT;
    applied_ = lp;
    applied_css_ = css;
    laid_out_ = true;
    return EB_OK;
}

// The bookmark of the page the UI shows, taken before a relayout; the UI's own mark wins, then the last delivered one.
uint64_t RenderWorker::shown_mark(const RenderWant& w) const
{
    if (w.mark != kNoAnchor)
        return w.mark;
    if (last_mark_ && last_page_ == w.page)
        return *last_mark_;
    uint64_t m = 0;
    if (laid_out_ && w.page >= 0 && w.page < count_ && eb_bookmark_of_page(doc_, w.page, &m) == EB_OK)
        return m;
    return kNoAnchor;
}

eb_status RenderWorker::ensure_layout(const RenderWant& w, int32_t& page)
{
    if (resolved_gen_ == w.generation) {
        page = resolved_page_;
        return EB_OK;
    }
    uint64_t mark = w.anchor;
    if (reflowable_) {
        const ReaderStyle& style = style_of(w);
        LayoutParams lp = reflow_layout(w.view.client_w, w.view.client_h, w.view.dpi);
        lp.em_pt = style.em_pt;
        if (!laid_out_ || !(lp == applied_) || style.css != applied_css_) {
            if (mark == kNoAnchor)
                mark = shown_mark(w);
            if (const eb_status st = apply_layout(lp, style.css); st != EB_OK)
                return st;
        }
    }
    page = w.page;
    if (mark != kNoAnchor) {
        if (const eb_status st = eb_page_of_bookmark(doc_, mark, &page); st != EB_OK)
            return st;
    }
    page = std::clamp(page, 0, count_ - 1);
    resolved_gen_ = w.generation;
    resolved_page_ = page;
    return EB_OK;
}

eb_status RenderWorker::serve_once(const RenderWant& w)
{
    int32_t page = 0;
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
    if (!w.have_next && page + 1 < count_) {
        if (deliver(w, page + 1, true, false) == EB_E_ABORTED)
            return EB_E_ABORTED;
    }
    if (!w.have_prev && page > 0)
        return deliver(w, page - 1, true, false) == EB_E_ABORTED ? EB_E_ABORTED : EB_OK;
    return EB_OK;
}

void RenderWorker::log_opts(const eb_render_opts& o)
{
    const std::array<uint32_t, 3> now{static_cast<uint32_t>(o.tint), o.fg_rgb, o.bg_rgb};
    if (now == logged_opts_)
        return;
    logged_opts_ = now;
    log::info("reader: render-opts tint={} fg={} bg={}", o.tint, format_hex_rgb(o.fg_rgb), format_hex_rgb(o.bg_rgb));
}

eb_status RenderWorker::render_bitmap(const RenderWant& w, int32_t page, RenderResult& r)
{
    float scale = render_scale(w.view.dpi);
    float pw = applied_.width_pt, ph = applied_.height_pt;
    if (!reflowable_) {
        if (const eb_status st = eb_page_size(doc_, page, &pw, &ph); st != EB_OK)
            return st;
        scale = fixed_scale(w.view.zoom, pw, ph, w.view.client_w, w.view.client_h, w.view.dpi, w.max_side_px);
    }
    scale = limit_scale(scale, pw, ph, w.max_side_px);
    const ReaderStyle& style = style_of(w);
    const eb_render_opts opts{scale, style.colors.fg, style.colors.bg, style.tint ? 1 : 0};
    log_opts(opts);
    eb_bitmap* b = nullptr;
    const eb_status st = eb_render_page(doc_, page, &opts, &b);
    r.bmp.reset(b);
    r.scale = scale;
    return st;
}

eb_status RenderWorker::deliver(const RenderWant& w, int32_t page, bool with_bitmap, bool primary)
{
    if (stop_.stop_requested())
        return EB_E_ABORTED;
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
        if (page + 1 < count_ && eb_bookmark_of_page(doc_, page + 1, &r.end_mark) != EB_OK)
            r.end_mark = r.mark + 1;
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
        last_page_ = page;
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
    RenderResult r;
    r.kind = RenderResult::Kind::Links;
    r.generation = w.generation;
    r.view_id = w.view_id;
    r.page = page;
    if (const eb_status st = load_links(page, r); st == EB_E_ABORTED)
        return st;
    if (!queue_.current(w.generation))
        return EB_E_ABORTED;
    post(std::move(r));
    links_gen_ = w.generation;
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
