#pragma once
#include "lib/LibraryModel.h"

#include <windows.h>

#include <commctrl.h>

#include <string>
#include <vector>

namespace eb {

struct TreeScope {
    enum class Kind : unsigned char { All, Reading, Folder };
    Kind kind = Kind::All;
    std::wstring prefix;
};

class FolderTree {
public:
    void attach(HWND tree) noexcept { tree_ = tree; }
    // Rebuilds the items and restores the previous selection when it still exists.
    void rebuild(const std::vector<FolderNode>& nodes);
    // Maps a TVN_SELCHANGED notification to the scope it selected and remembers it.
    bool on_select(const NMTREEVIEWW& nm);
    [[nodiscard]] const TreeScope& current() const noexcept { return current_; }

private:
    HTREEITEM insert(HTREEITEM parent, const std::wstring& text, size_t index);

    HWND tree_ = nullptr;
    std::vector<TreeScope> scopes_;
    TreeScope current_;
    bool rebuilding_ = false;
};

}
