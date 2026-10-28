#include "TocPane.h"

#include "base/Text.h"

#include <commctrl.h>

#include <algorithm>
#include <format>

namespace eb {
namespace {

constexpr LPARAM kRootParam = -1;

std::wstring clean_label(std::wstring s)
{
    std::ranges::replace_if(s, [](wchar_t c) { return c < L' '; }, L' ');
    return s;
}

HTREEITEM insert(HWND tree, HTREEITEM parent, const std::wstring& text, LPARAM param)
{
    TVINSERTSTRUCTW ins{};
    ins.hParent = parent;
    ins.hInsertAfter = TVI_LAST;
    ins.item.mask = TVIF_TEXT | TVIF_PARAM;
    ins.item.pszText = const_cast<wchar_t*>(text.c_str());
    ins.item.lParam = param;
    return TreeView_InsertItem(tree, &ins);
}

}

bool TocPane::create(HINSTANCE inst, HWND parent, int id) noexcept
{
    tree_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
        WS_CHILD | WS_TABSTOP | WS_CLIPSIBLINGS | TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS | TVS_SHOWSELALWAYS, 0, 0, 0, 0,
        parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr); // NOLINT(performance-no-int-to-ptr): Win32 message ABI
    return tree_ != nullptr;
}

void TocPane::set_content(const std::vector<TocItem>& toc, const std::vector<Bookmark>& marks)
{
    SendMessageW(tree_, WM_SETREDRAW, FALSE, 0);
    TreeView_DeleteAllItems(tree_);
    picks_.clear();
    HTREEITEM toc_root = insert(tree_, TVI_ROOT, std::format(L"Mục lục ({})", toc.size()), kRootParam);
    std::vector<int32_t> levels;
    levels.reserve(toc.size());
    for (const TocItem& t : toc)
        levels.push_back(t.level);
    const std::vector<int> parents = toc_parents(levels);
    std::vector<HTREEITEM> items;
    items.reserve(toc.size());
    for (std::size_t i = 0; i < toc.size(); ++i) {
        picks_.push_back({toc[i].target, 0});
        HTREEITEM up = parents[i] < 0 ? toc_root : items[static_cast<std::size_t>(parents[i])];
        const std::wstring title = clean_label(utf8_to_wide(toc[i].title).value_or(std::wstring{}));
        items.push_back(insert(tree_, up, title, static_cast<LPARAM>(picks_.size() - 1)));
    }
    HTREEITEM mark_root = insert(tree_, TVI_ROOT, std::format(L"Dấu trang ({})", marks.size()), kRootParam);
    for (const Bookmark& b : marks) {
        picks_.push_back({b.mark, b.id});
        insert(tree_, mark_root, clean_label(b.label), static_cast<LPARAM>(picks_.size() - 1));
    }
    TreeView_Expand(tree_, toc_root, TVE_EXPAND);
    TreeView_Expand(tree_, mark_root, TVE_EXPAND);
    TreeView_EnsureVisible(tree_, toc_root);
    SendMessageW(tree_, WM_SETREDRAW, TRUE, 0);
}

std::optional<TocPane::Pick> TocPane::selected() const
{
    TVITEMW it{};
    it.mask = TVIF_PARAM;
    it.hItem = TreeView_GetSelection(tree_);
    if (it.hItem == nullptr || !TreeView_GetItem(tree_, &it) || it.lParam < 0 || static_cast<std::size_t>(it.lParam) >= picks_.size())
        return std::nullopt;
    return picks_[static_cast<std::size_t>(it.lParam)];
}

}
