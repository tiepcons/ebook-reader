#include "ReaderWorker.h"

#include "ReaderSpread.h"
#include "base/Log.h"

#include <chrono>
#include <utility>

namespace eb {
namespace {

// While the book is laid out chapter by chapter the view hears about the new numbering at most this often.
constexpr long long kPublishEveryMs = 300;
constexpr int kMaxSettle = 3;

}

eb_status RenderWorker::apply_layout(const LayoutParams& lp, const std::string& css)
{
    layout_t0_ = std::chrono::steady_clock::now();
    if (const eb_status st = eb_layout_lazy(doc_, lp.width_pt, lp.height_pt, lp.em_pt, css.empty() ? nullptr : css.c_str());
        st != EB_OK)
        return st;
    if (const eb_status st = eb_page_count(doc_, &count_); st != EB_OK)
        return st;
    if (count_ <= 0)
        return EB_E_FORMAT;
    applied_ = lp;
    applied_css_ = css;
    laid_out_ = true;
    layout_gen_ = eb_layout_generation(doc_);
    complete_ = false;
    ref_valid_ = false;
    numbering_dirty_ = false;
    steps_ = 0;
    return EB_OK;
}

void RenderWorker::save_cache() noexcept
{
    if (cache_.empty() || (saved_ && saved_->first == applied_ && saved_->second == applied_css_))
        return;
    const eb_status st = eb_save_cache(doc_, cache_.c_str());
    if (st == EB_OK) {
        try {
            saved_.emplace(applied_, applied_css_);
        } catch (...) {
            saved_.reset();
        }
        return;
    }
    if (st == EB_E_FORMAT || cache_failed_)
        return;
    cache_failed_ = true;
    log::error("reader: layout cache not saved: {}", eb_status_text(st));
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
        LayoutParams lp = reflow_layout(slot_width_px(w.view.client_w, w.view.dpi, w.view.pages), w.view.client_h, w.view.dpi);
        lp.em_pt = style.em_pt;
        if (!laid_out_ || !(lp == applied_) || style.css != applied_css_) {
            relayout_mark_.reset();
            if (const uint64_t shown = shown_mark(w); shown != kNoAnchor)
                relayout_mark_ = shown;
            if (const eb_status st = apply_layout(lp, style.css); st != EB_OK)
                return st;
        }
    }
    if (mark == kNoAnchor && !ref_valid_ && relayout_mark_)
        mark = *relayout_mark_;
    page = w.page;
    if (mark != kNoAnchor) {
        for (int again = 0; again < kMaxSettle; ++again) {
            if (const eb_status st = eb_page_of_bookmark(doc_, mark, &page); st != EB_OK)
                return st;
            const Moved moved = numbering_moved();
            if (moved == Moved::Bumped)
                return EB_E_ABORTED;
            if (moved == Moved::None)
                break;
        }
    }
    page = spread_start(page, count_, w.view.pages);
    resolved_gen_ = w.generation;
    resolved_page_ = page;
    return EB_OK;
}

void RenderWorker::note_generation()
{
    const uint32_t gen = eb_layout_generation(doc_);
    if (gen == layout_gen_)
        return;
    layout_gen_ = gen;
    numbering_dirty_ = true;
    if (int32_t n = 0; eb_page_count(doc_, &n) == EB_OK && n > 0)
        count_ = n;
}

RenderWorker::Moved RenderWorker::flush_numbering()
{
    if (!std::exchange(numbering_dirty_, false))
        return Moved::None;
    return ref_valid_ && publish_layout(true, false) ? Moved::Bumped : Moved::Quiet;
}

RenderWorker::Moved RenderWorker::numbering_moved()
{
    note_generation();
    return flush_numbering();
}

// Posts the new page count; the numbering only changes (and pending requests only go stale) when the pages up to the one
// delivered last moved.
bool RenderWorker::publish_layout(bool check_shift, bool toc_changed)
{
    RenderResult r;
    r.kind = RenderResult::Kind::Layout;
    r.page_count = count_;
    r.complete = complete_;
    r.toc_changed = toc_changed;
    if (int32_t p = 0; check_shift && last_mark_ && eb_page_of_bookmark(doc_, *last_mark_, &p) == EB_OK) {
        r.shift = p - last_page_;
        last_page_ = p;
    }
    const bool bumped = r.shift != 0;
    if (bumped) {
        ++numbering_;
        r.requested_mark = bail_mark_.value_or(UINT64_MAX);
    }
    bail_mark_.reset();
    r.page = last_page_;
    r.numbering = numbering_;
    published_ = std::chrono::steady_clock::now();
    post(std::move(r));
    return bumped;
}

void RenderWorker::finish_layout()
{
    complete_ = true;
    log::info("layout: complete-ms {} steps={} pages={}", ms_since(layout_t0_), steps_, count_);
    flush_numbering();
    save_cache();
    toc_loaded_ = false;
    publish_layout(false, load_toc());
}

// One chapter of the book is laid out, nearest to the reading position first; a request that arrives meanwhile aborts the step.
void RenderWorker::run_idle() noexcept
{
    try {
        int32_t done = 0;
        const eb_status st = eb_layout_step(doc_, last_mark_.value_or(0), &done);
        ++steps_;
        if (st != EB_OK && st != EB_E_ABORTED) {
            log::error("layout: step failed: {}", eb_status_text(st));
            complete_ = true;
            publish_layout(false, false);
            return;
        }
        note_generation();
        if (st == EB_OK && done != 0)
            finish_layout();
        else if (numbering_dirty_ && ms_since(published_) >= kPublishEveryMs)
            flush_numbering();
    } catch (...) {
        log::error("layout: step failed");
        complete_ = true;
    }
}

}
