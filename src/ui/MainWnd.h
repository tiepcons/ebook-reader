#pragma once
#include "FolderTree.h"
#include "LibraryView.h"
#include "ScanWorker.h"
#include "UiMessages.h"
#include "base/Handle.h"
#include "base/Paths.h"
#include "lib/Db.h"

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace eb {

struct FontTraits {
    using Raw = HFONT;
    static bool valid(HFONT f) noexcept { return f != nullptr; }
    static void close(HFONT f) noexcept { DeleteObject(f); }
};
using UniqueFont = Unique<FontTraits>;

class MainWnd {
public:
    MainWnd(HINSTANCE inst, Db& db, const AppPaths& paths, bool smoke, bool smoke_scan,
        std::chrono::steady_clock::time_point start) noexcept
        : inst_(inst), db_(db), paths_(paths), smoke_(smoke), smoke_scan_(smoke_scan), start_(start), lib_(db)
    {
    }
    MainWnd(const MainWnd&) = delete;
    MainWnd& operator=(const MainWnd&) = delete;
    ~MainWnd();

    bool create(int show) noexcept;
    [[nodiscard]] HWND hwnd() const noexcept { return hwnd_; }

private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) noexcept;
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

    bool create_children();
    void update_font();
    void layout();
    int px(int dip) const noexcept;
    void restore_placement(int show);
    void save_state();
    void on_command(WPARAM wp);
    void on_paint();
    bool on_splitter_msg(UINT msg, WPARAM wp, LPARAM lp, LRESULT& res);
    void on_dpi_changed(WPARAM wp, LPARAM lp);
    void splitter_drag(int x_client);
    void open_ebooks_folder();

    bool on_library_msg(UINT msg, WPARAM wp, LPARAM lp, LRESULT& res);
    void start_scan();
    void on_first_paint();
    void on_scan_progress(WPARAM done, LPARAM total);
    void on_scan_done(WPARAM result);
    void on_notify(NMHDR& hdr, LRESULT& result);
    void on_context_menu(LPARAM lp);
    void request_open(const std::string& id);
    [[nodiscard]] std::vector<std::filesystem::path> library_roots();
    void refresh_tree();
    void refresh_list();
    void update_status();
    [[nodiscard]] Query current_query() const;

    HINSTANCE inst_;
    Db& db_;
    const AppPaths& paths_;
    bool smoke_;
    bool smoke_scan_;
    std::chrono::steady_clock::time_point start_;
    bool painted_ = false;
    bool dragging_ = false;
    HWND hwnd_ = nullptr;
    HWND search_ = nullptr, sort_ = nullptr, rescan_ = nullptr, open_ = nullptr;
    HWND tree_ = nullptr, list_ = nullptr, status_ = nullptr;
    RECT split_rc_{};
    UINT dpi_ = 96;
    int split_dip_ = 220;
    UniqueFont font_;
    LibraryView lib_;
    FolderTree ftree_;
    ScanWorker scan_;
    std::chrono::steady_clock::time_point last_scan_{};
    int scan_percent_ = -1;
    bool scan_dirty_ = false;
    std::vector<std::wstring> tree_folders_, tree_roots_;
    std::vector<std::string> open_queue_;
};

}
