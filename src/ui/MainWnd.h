#pragma once
#include "CloseAsk.h"
#include "ExternalOpen.h"
#include "FolderTree.h"
#include "LibraryView.h"
#include "ReaderView.h"
#include "ScanWorker.h"
#include "SingleInstance.h"
#include "Tray.h"
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
        std::chrono::steady_clock::time_point start)
        : inst_(inst), db_(db), paths_(paths), smoke_(smoke), smoke_scan_(smoke_scan), start_(start), lib_(db), reader_(db)
    {
    }
    MainWnd(const MainWnd&) = delete;
    MainWnd& operator=(const MainWnd&) = delete;
    ~MainWnd();

    bool create(int show) noexcept;
    [[nodiscard]] HWND hwnd() const noexcept { return hwnd_; }
    void set_launch(LaunchArgs args, std::uint64_t key) { launch_ = std::move(args); instance_key_ = key; }

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
    bool on_worker_msg(UINT msg, WPARAM wp, LPARAM lp);
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

    bool on_reader_msg(UINT msg, WPARAM wp, LPARAM lp, LRESULT& res);
    bool create_reader_children();
    void layout_reader();
    void open_book(const std::string& id);
    void close_book();
    void on_reader_event(ReaderEvent e, int page);
    void toggle_fullscreen();
    void show_resume(int percent, int page);
    void hide_resume();
    void show_notice(const wchar_t* text);
    void init_smoke_open();
    void smoke_open_book();
    void smoke_step(int page);

    bool on_tray_msg(UINT msg, WPARAM wp, LPARAM lp, LRESULT& res);
    bool on_tray_command(int id);
    bool on_tray_timer(UINT_PTR id);
    void on_close_request();
    void on_minimized();
    void exit_app();
    void send_to_tray();
    void bring_to_front();
    bool on_copydata(LPARAM lp);
    void on_open_path();
    void on_external_meta();
    void open_external_id(const std::string& id);
    void init_launch();
    void after_first_paint_tray();
    void shutdown_tray();
    void smoke_tray_check();

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
    ReaderView reader_;
    HWND back_ = nullptr, resume_text_ = nullptr, resume_btn_ = nullptr;
    bool in_reader_ = false, fullscreen_ = false, smoke_keys_sent_ = false;
    int resume_page_ = -1;
    std::string smoke_open_, reading_id_;
    WINDOWPLACEMENT saved_wp_{};
    LONG_PTR saved_style_ = 0;
    HMENU saved_menu_ = nullptr;
    Tray tray_;
    ExternalOpen ext_;
    LaunchArgs launch_;
    std::uint64_t instance_key_ = 0;
    std::wstring pending_path_;
    bool in_tray_ = false, asking_ = false;
};

}
