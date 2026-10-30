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
    const bool pair = has_right_page(page_, count_, view_.pages);
    const wchar_t* more = layout_complete_ ? L"" : L"…";
    std::wstring t = pair ? std::format(L"Trang {}–{} / {}{} · {} %", page_ + 1, page_ + 2, count_, more, percent())
                          : std::format(L"Trang {} / {}{} · {} %", page_ + 1, count_, more, percent());
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

namespace {

std::vector<std::size_t> bookmarks_on(const std::vector<Bookmark>& all, uint64_t start, uint64_t end)
{
    std::vector<uint64_t> marks;
    marks.reserve(all.size());
    for (const Bookmark& b : all)
        marks.push_back(b.mark);
    return marks_on_page(marks, start, end);
}

}

bool ReaderView::page_bookmarked() const
{
    if (!worker_ || mark_ == kNoAnchor)
        return false;
    if (marks_dirty_) {
        marks_cache_.clear();
        for (const Bookmark& b : bookmarks())
            marks_cache_.push_back(b.mark);
        marks_dirty_ = false;
    }
    return !marks_on_page(marks_cache_, mark_, end_mark_).empty();
}

std::optional<bool> ReaderView::toggle_bookmark()
{
    if (!worker_ || mark_ == kNoAnchor)
        return std::nullopt;
    const std::vector<Bookmark> all = bookmarks();
    const std::vector<std::size_t> here = bookmarks_on(all, mark_, end_mark_);
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
