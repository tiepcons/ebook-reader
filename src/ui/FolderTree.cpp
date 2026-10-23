#include "FolderTree.h"

#include <commctrl.h>

namespace eb {

HTREEITEM FolderTree::insert(HTREEITEM parent, const std::wstring& text, size_t index)
{
    TVINSERTSTRUCTW ins{};
    ins.hParent = parent;
    ins.hInsertAfter = TVI_LAST;
    ins.item.mask = TVIF_TEXT | TVIF_PARAM;
    ins.item.pszText = const_cast<wchar_t*>(text.c_str());
    ins.item.lParam = static_cast<LPARAM>(index);
    return TreeView_InsertItem(tree_, &ins);
}

void FolderTree::rebuild(const std::vector<FolderNode>& nodes)
{
    const TreeScope keep = current_;
    rebuilding_ = true;
    SendMessageW(tree_, WM_SETREDRAW, FALSE, 0);
    TreeView_DeleteAllItems(tree_);
    scopes_.clear();
    scopes_.push_back({TreeScope::Kind::All, {}});
    scopes_.push_back({TreeScope::Kind::Reading, {}});
    HTREEITEM select = insert(TVI_ROOT, L"Tất cả sách", 0);
    insert(TVI_ROOT, L"Đang đọc dở", 1);
    if (keep.kind == TreeScope::Kind::Reading)
        select = TreeView_GetNextSibling(tree_, select);
    std::vector<HTREEITEM> items;
    items.reserve(nodes.size());
    for (const auto& n : nodes) {
        const auto parent = n.parent >= 0 ? items[static_cast<size_t>(n.parent)] : TVI_ROOT;
        scopes_.push_back({TreeScope::Kind::Folder, n.prefix});
        const auto it = insert(parent, n.label, scopes_.size() - 1);
        items.push_back(it);
        if (keep.kind == TreeScope::Kind::Folder && CompareStringOrdinal(n.prefix.c_str(), -1, keep.prefix.c_str(), -1, TRUE) == CSTR_EQUAL)
            select = it;
    }
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].parent < 0)
            TreeView_Expand(tree_, items[i], TVE_EXPAND);
    }
    TreeView_SelectItem(tree_, select);
    SendMessageW(tree_, WM_SETREDRAW, TRUE, 0);
    rebuilding_ = false;
    TVITEMW got{};
    got.mask = TVIF_PARAM;
    got.hItem = select;
    current_ = TreeView_GetItem(tree_, &got) && static_cast<size_t>(got.lParam) < scopes_.size()
        ? scopes_[static_cast<size_t>(got.lParam)]
        : TreeScope{};
}

bool FolderTree::on_select(const NMTREEVIEWW& nm)
{
    if (rebuilding_ || nm.itemNew.lParam < 0 || static_cast<size_t>(nm.itemNew.lParam) >= scopes_.size())
        return false;
    current_ = scopes_[static_cast<size_t>(nm.itemNew.lParam)];
    return true;
}

}
