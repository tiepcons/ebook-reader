#pragma once
#include "ReaderLogic.h"
#include "lib/ProgressStore.h"

#include <windows.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace eb {

// Side pane with one tree: the book outline and the bookmarks. Entries are looked up by position, not stored in the control.
class TocPane {
public:
    struct Pick {
        uint64_t mark = 0;
        int64_t bookmark_id = 0; // 0 = outline entry
    };

    bool create(HINSTANCE inst, HWND parent, int id) noexcept;
    [[nodiscard]] HWND hwnd() const noexcept { return tree_; }
    void set_content(const std::vector<TocItem>& toc, const std::vector<Bookmark>& marks);
    [[nodiscard]] std::optional<Pick> selected() const;

private:
    HWND tree_ = nullptr;
    std::vector<Pick> picks_;
};

}
