#pragma once
#include "Covers.h"
#include "base/Paths.h"
#include "lib/CoverCache.h"
#include "lib/Db.h"
#include "lib/LibraryModel.h"

#include <windows.h>

#include <commctrl.h>

#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace eb {

enum class MenuOutcome : unsigned char { None, Open, Removed };

// The owner-data book grid. Rows are served from a cached copy of the last query.
class LibraryView {
public:
    explicit LibraryView(Db& db) : db_(db) {}
    ~LibraryView();
    LibraryView(const LibraryView&) = delete;
    LibraryView& operator=(const LibraryView&) = delete;

    bool attach(HWND list, UINT dpi, const AppPaths& paths);
    // Loads finished covers into the image list; call on WM_APP_COVER_READY.
    void on_cover_ready();
    // Requests the covers of the first n rows regardless of visibility (smoke test).
    void prefetch_covers(size_t n);
    [[nodiscard]] bool covers_idle() const noexcept { return pending_.empty(); }
    // Redraws the icons for the dpi / system colours and hands them to the list.
    void rebuild_covers(UINT dpi, bool force);
    bool refresh(const Query& q);
    [[nodiscard]] size_t count() const noexcept { return rows_.size(); }

    void on_getdispinfo(NMLVDISPINFOW& di);
    bool on_custom_draw(NMLVCUSTOMDRAW& cd, LRESULT& result) const;
    [[nodiscard]] std::optional<std::string> selected_id() const;
    // lp is the WM_CONTEXTMENU position, or -1 for the keyboard. Open yields the id through open_id.
    MenuOutcome context_menu(LPARAM lp, std::string& open_id);

private:
    [[nodiscard]] const BookRow* selected_row() const;
    [[nodiscard]] const std::wstring& label(size_t i);
    void request_cover(const BookRow& row);
    void reveal(const BookRow& row) const;
    bool confirm_remove(const BookRow& row) const;

    Db& db_;
    HWND list_ = nullptr;
    std::vector<BookRow> rows_;
    Covers covers_;
    std::unique_ptr<CoverCache> cache_;
    std::unique_ptr<CoverWorker> worker_;
    std::unordered_set<std::string> pending_, failed_;
    std::vector<std::wstring> text_; // lazily built label per row; empty = not built yet
};

}
