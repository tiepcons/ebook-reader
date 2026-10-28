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
    highlight_.clear();
    highlight_page_ = tts_page_ = -1;
    tts_char_ = -1;
    relaid_ = true;
    notify(ReaderEvent::Relayout);
}

void ReaderView::request_text(int32_t page)
{
    if (worker_)
        worker_->request_text(page);
}

void ReaderView::store_text(const RenderResult& r)
{
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
    if (highlight_page_ != page_ || scale_ <= 0)
        return out;
    const auto ox = static_cast<float>(origin_x), oy = static_cast<float>(origin_y);
    for (const TextBox& b : highlight_)
        out.push_back(D2D1::RectF(ox + b.x0 * scale_, oy + b.y0 * scale_, ox + b.x1 * scale_, oy + b.y1 * scale_));
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
    if (saver_ && page == page_ && mark_ != kNoAnchor)
        saver_->update(Progress{mark_, reading_fraction(page_, count_), tts_char_, unix_now()});
}

int64_t ReaderView::resume_tts_char() const noexcept
{
    if (tts_page_ == page_ && tts_char_ >= 0)
        return tts_char_;
    return mark_ != kNoAnchor && mark_ == saved_mark_ ? saved_tts_char_ : -1;
}

}
