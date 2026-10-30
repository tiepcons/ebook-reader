#include "ReaderView.h"

#include <algorithm>
#include <chrono>

namespace eb {
namespace {

constexpr std::size_t kKeptTexts = 3;

int64_t unix_now() noexcept
{
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

}

void ReaderView::relayout()
{
    ++view_id_;
    if (!reflowable_)
        return;
    texts_.clear();
    pending_texts_.clear();
    links_.clear();
    links_page_ = links_right_page_ = -1;
    links_right_.clear();
    highlight_.clear();
    highlight_page_ = tts_page_ = focus_page_ = -1;
    tts_char_ = -1;
    relaid_ = true;
    notify(ReaderEvent::Relayout);
}

// The book was laid out further in the background: `shift` is how far the numbers of the pages shown moved.
void ReaderView::on_layout(const RenderResult& r)
{
    const bool bumped = r.numbering != numbering_;
    const int32_t old_count = count_;
    numbering_ = r.numbering;
    layout_complete_ = r.complete;
    if (r.toc_changed && worker_) {
        toc_ = worker_->toc();
        ++content_version_;
    }
    count_ = r.page_count;
    if (bumped)
        renumber(r.shift, old_count);
    else
        follow_count(old_count);
    if (bumped && r.requested_mark != kNoAnchor && mark_ == kNoAnchor)
        anchor_ = r.requested_mark;
    if (saver_ && mark_ != kNoAnchor)
        saver_->update(Progress{mark_, reading_fraction(page_, count_), tts_page_ == page_ ? tts_char_ : -1, unix_now()});
    SendMessageW(parent_, WM_APP_READER_EVENT, static_cast<WPARAM>(ReaderEvent::Renumbered), static_cast<LPARAM>(r.shift));
    if (!bumped || !worker_)
        return;
    for (const int32_t page : pending_texts_)
        worker_->request_text(page, numbering_);
    send_want();
}

// Only the page count changed: the pages around the shown one keep their numbers, the ones further away may not.
void ReaderView::follow_count(int32_t old_count)
{
    if (count_ == old_count)
        return;
    if (mark_ == kNoAnchor && page_ >= spread_start(old_count - 1, old_count, view_.pages)) {
        page_ = spread_start(count_ - 1, count_, view_.pages); // a jump to the end, whose chapter was laid out meanwhile
        send_want();
    }
    cache_.keep_only(page_ - span(), page_ + 2 * span() - 1);
}

void ReaderView::renumber(int32_t shift, int32_t old_count)
{
    const auto moved = [&](int32_t& p) {
        if (p >= 0)
            p = std::clamp(p + shift, 0, std::max(count_ - 1, 0));
    };
    const int32_t reading = reading_page();
    page_ = renumbered_page(page_, shift, old_count, count_, view_.pages);
    focus_page_ = renumbered_focus(reading, shift, page_, count_, view_.pages);
    moved(tts_page_);
    moved(highlight_page_);
    for (int32_t& p : pending_texts_)
        moved(p);
    for (auto& t : texts_) {
        auto copy = std::make_shared<PageText>(*t);
        copy->page = std::max(t->page + shift, 0);
        t = std::move(copy);
    }
    links_.clear();
    links_right_.clear();
    links_page_ = links_right_page_ = -1;
    rekey_cache(shift);
}

void ReaderView::request_text(int32_t page)
{
    if (!worker_)
        return;
    note_pending_text(pending_texts_, page);
    worker_->request_text(page, numbering_);
}

void ReaderView::store_text(const RenderResult& r)
{
    std::erase(pending_texts_, r.page);
    if (r.text) {
        std::erase_if(texts_, [&](const auto& t) { return t->page == r.page; });
        if (texts_.size() >= kKeptTexts)
            texts_.erase(texts_.begin());
        texts_.push_back(r.text);
    }
    PostMessageW(parent_, WM_APP_READER_EVENT, static_cast<WPARAM>(ReaderEvent::Text), static_cast<LPARAM>(r.page));
}

std::shared_ptr<const PageText> ReaderView::text_of(int32_t page) const
{
    const auto it = std::ranges::find_if(texts_, [&](const auto& t) { return t->page == page; });
    return it != texts_.end() ? *it : nullptr;
}

std::size_t ReaderView::set_highlight(int32_t page, uint32_t begin, uint32_t end)
{
    const auto t = text_of(page);
    highlight_ = t ? sentence_lines(*t, begin, end) : std::vector<TextBox>{};
    highlight_page_ = page;
    if (hwnd_ != nullptr)
        InvalidateRect(hwnd_, nullptr, FALSE);
    return highlight_.size();
}

std::vector<D2D1_RECT_F> ReaderView::highlight_rects(int origin_x, int origin_y) const
{
    std::vector<D2D1_RECT_F> out;
    const SpreadLayout lay = layout_now();
    const bool right = spread() && highlight_page_ == page_ + 1;
    const float scale = right ? scale_right_ : scale_;
    if ((highlight_page_ != page_ && !right) || scale <= 0)
        return out;
    const auto ox = static_cast<float>(origin_x + (right ? lay.right_x : lay.left_x));
    const auto oy = static_cast<float>(origin_y + (right ? lay.right_y : lay.left_y));
    for (const TextBox& b : highlight_)
        out.push_back(D2D1::RectF(ox + b.x0 * scale, oy + b.y0 * scale, ox + b.x1 * scale, oy + b.y1 * scale));
    return out;
}

void ReaderView::clear_highlight()
{
    highlight_.clear();
    highlight_page_ = -1;
    if (hwnd_ != nullptr)
        InvalidateRect(hwnd_, nullptr, FALSE);
}

void ReaderView::save_tts_position(int32_t page, uint32_t offset)
{
    tts_page_ = page;
    tts_char_ = offset;
    if (!saver_)
        return;
    const bool right = spread() && page == page_ + 1;
    const uint64_t mark = right ? end_mark_ : mark_;
    if ((page == page_ || right) && mark != kNoAnchor)
        saver_->update(Progress{mark, reading_fraction(page, count_), tts_char_, unix_now()});
}

int64_t ReaderView::resume_tts_char() const noexcept
{
    const int32_t reading = reading_page();
    if (tts_page_ == reading && tts_char_ >= 0)
        return tts_char_;
    const uint64_t mark = reading == page_ ? mark_ : end_mark_;
    return mark != kNoAnchor && mark == saved_mark_ ? saved_tts_char_ : -1;
}

}
