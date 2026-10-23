#pragma once
#include "Covers.h"
#include "lib/Db.h"
#include "lib/LibraryModel.h"

#include <windows.h>

#include <commctrl.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace eb {

enum class MenuOutcome : unsigned char { None, Open, Removed };

// The owner-data book grid. Rows are served from a cached copy of the last query.
class LibraryView {
public:
    explicit LibraryView(Db& db) noexcept : db_(db) {}

    bool attach(HWND list, UINT dpi);
    void set_dpi(UINT dpi);
    bool refresh(const Query& q);
    [[nodiscard]] size_t count() const noexcept { return rows_.size(); }

    void on_getdispinfo(NMLVDISPINFOW& di) const;
    bool on_custom_draw(NMLVCUSTOMDRAW& cd, LRESULT& result) const;
    [[nodiscard]] std::optional<std::string> selected_id() const;
    // lp is the WM_CONTEXTMENU position, or -1 for the keyboard. Open yields the id through open_id.
    MenuOutcome context_menu(LPARAM lp, std::string& open_id);

private:
    [[nodiscard]] const BookRow* selected_row() const;
    void reveal(const BookRow& row) const;
    bool confirm_remove(const BookRow& row) const;

    Db& db_;
    HWND list_ = nullptr;
    std::vector<BookRow> rows_;
    std::unique_ptr<CoverProvider> covers_;
};

}
