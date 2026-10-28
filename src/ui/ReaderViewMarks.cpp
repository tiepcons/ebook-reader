#include "ReaderView.h"

#include <cmath>
#include <format>

namespace eb {

int ReaderView::percent() const noexcept
{
    return count_ <= 0 ? 0 : static_cast<int>(std::lround(100.0 * reading_fraction(page_, count_)));
}

std::wstring ReaderView::status_text() const
{
    if (count_ <= 0)
        return note_.empty() ? std::wstring(L"Đang mở…") : note_;
    std::wstring t = std::format(L"Trang {} / {} · {} %", page_ + 1, count_, percent());
    if (!chapter_.empty())
        t += L" · " + chapter_;
    if (!note_.empty())
        t += L" · " + note_;
    return t;
}

void ReaderView::go_to_mark(uint64_t mark)
{
    if (!worker_ || count_ <= 0)
        return;
    anchor_ = mark;
    mark_ = kNoAnchor;
    pan_x_ = pan_y_ = 0;
    send_want();
}

void ReaderView::go_to_percent(int percent)
{
    go_to(page_for_percent(percent, count_));
}

std::vector<Bookmark> ReaderView::bookmarks() const
{
    if (book_id_.empty())
        return {};
    auto list = list_bookmarks(db_, book_id_);
    return list ? std::move(*list) : std::vector<Bookmark>{};
}

std::optional<bool> ReaderView::toggle_bookmark()
{
    if (!worker_ || mark_ == kNoAnchor)
        return std::nullopt;
    const std::vector<Bookmark> all = bookmarks();
    std::vector<uint64_t> marks;
    marks.reserve(all.size());
    for (const Bookmark& b : all)
        marks.push_back(b.mark);
    const std::vector<std::size_t> here = marks_on_page(marks, mark_, end_mark_);
    ++content_version_;
    if (here.empty()) {
        const std::wstring label = std::format(L"{} % · {}", percent(), chapter_.empty() ? std::format(L"Trang {}", page_ + 1) : chapter_);
        return add_bookmark(db_, book_id_, mark_, label) ? std::optional<bool>(true) : std::nullopt;
    }
    bool ok = true;
    for (const std::size_t i : here)
        ok = ::eb::remove_bookmark(db_, all[i].id).has_value() && ok;
    return ok ? std::optional<bool>(false) : std::nullopt;
}

bool ReaderView::remove_bookmark(int64_t id)
{
    ++content_version_;
    return ::eb::remove_bookmark(db_, id).has_value();
}

}
