#pragma once
#include "CloseAsk.h"
#include "ExternalOpen.h"
#include "FolderTree.h"
#include "LibraryView.h"
#include "ReaderView.h"
#include "ScanWorker.h"
#include "SingleInstance.h"
#include "TocPane.h"
#include "Tray.h"
#include "TtsBar.h"
#include "TtsController.h"
#include "TtsEngines.h"
#include "TtsSettings.h"
#include "UiMessages.h"
#include "base/Handle.h"
#include "base/Paths.h"
#include "lib/Db.h"

#include <windows.h>

#include <chrono>
#include <deque>
#include <span>
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

class MainWnd : private TtsHost, private TtsDialogApi {
public:
    MainWnd(HINSTANCE inst, Db& db, const AppPaths& paths, bool smoke, bool smoke_scan,
        std::chrono::steady_clock::time_point start)
        : inst_(inst), db_(db), paths_(paths), smoke_(smoke), smoke_scan_(smoke_scan), start_(start), lib_(db), reader_(db)
    {
    }
    MainWnd(const MainWnd&) = delete;
    MainWnd& operator=(const MainWnd&) = delete;
    ~MainWnd() override;

    bool create(int show) noexcept;
    [[nodiscard]] HWND hwnd() const noexcept { return hwnd_; }
    void set_tts_state(TtsState s);
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
    void init_soak();
    void soak_tick();
    void soak_log();
    void open_reader_settings();
    void apply_chrome();
    bool on_reader_command(WPARAM wp);
    void on_open_failed();
    void on_shortcut_event(ReaderEvent e);
    void on_toc_notify(NMHDR& hdr);
    void toggle_toc_pane();
    void refresh_toc_pane();
    void toggle_bookmark();
    void ask_goto();
    void jump_to_pick();
    void jump_to_mark(uint64_t mark);
    void on_page_event(ReaderEvent e, int page);
    void sync_chapter_combo();
    void fill_chapter_combo(std::span<const TocItem> toc);
    void on_chapter_combo(int code);
    bool create_bar_buttons();
    void layout_top_bar(const RECT& rc, int left);
    void update_bar_buttons();
    void reset_bar_marked();
    [[nodiscard]] int remembered_toc_index() const noexcept;
    void jump_to_toc(int idx);
    bool on_bar_command(int id);
    void smoke_marks();
    [[nodiscard]] bool toc_shown() const noexcept { return in_reader_ && toc_visible_ && !fullscreen_; }

    bool on_tray_msg(UINT msg, WPARAM wp, LPARAM lp, LRESULT& res);
    bool on_tray_command(int id);
    bool on_tray_timer(UINT_PTR id);
    void on_close_request();
    void on_minimized();
    void exit_app();
    void send_to_tray();
    void bring_to_front();
    bool on_copydata(LPARAM lp);
    bool sync_tray();
    void queue_open_path(std::wstring path);
    void pump_open_queue();
    void on_external_meta();
    void open_external_id(const std::string& id);
    void init_launch();
    void after_first_paint_tray();
    void shutdown_tray();
    void smoke_tray_check();

    bool ensure_tts();
    void load_tts_prefs_once();
    void tts_toggle();
    void tts_stop();
    void on_tts_text(int page);
    void on_tts_reader_event(ReaderEvent e, int page);
    bool on_tts_command(WPARAM wp);
    bool tts_button(int id);
    bool on_tts_msg(UINT msg, WPARAM wp, LPARAM lp);
    void on_volume_scroll(int code);
    void open_tts_settings();
    void refresh_azure_quota();
    void on_azure_warn(Warn level);
    void rebuild_azure_engine();
    void smoke_tts_event(uint32_t id, LPARAM lp);
    void layout_tts_bar(const RECT& rc, int status_h, int margin, int bar_h);
    [[nodiscard]] std::string book_lang_tag() const;
    void request_text(int32_t page) override;
    [[nodiscard]] int32_t shown_page() const noexcept override;
    [[nodiscard]] bool shown_has_right_page() const noexcept override;
    [[nodiscard]] int32_t page_count() const noexcept override;
    [[nodiscard]] int64_t resume_offset() const noexcept override;
    void turn_to(int32_t page) override;
    void highlight(int32_t page, uint32_t begin, uint32_t end) override;
    void clear_highlight() override;
    void save_position(int32_t page, uint32_t offset) override;
    void state_changed(TtsState s) override;
    void notice(const wchar_t* text) override;
    std::vector<TtsKind> available() override;
    std::vector<Voice> voices(TtsKind k) override;
    void preview(TtsKind k, const std::wstring& voice, Lang lang, double rate) override;
    void end_preview() override;
    AzureView azure_view() override;
    bool azure_save(const AzureEdits& e) override;
    bool azure_reset_used() override;

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
    TocPane toc_pane_;
    HWND back_ = nullptr, resume_text_ = nullptr, resume_btn_ = nullptr, aa_ = nullptr, chapter_combo_ = nullptr;
    HWND chapter_prev_ = nullptr, chapter_next_ = nullptr, toc_btn_ = nullptr, mark_btn_ = nullptr, tips_ = nullptr;
    bool bar_marked_ = false, nav_pending_ = false;
    int nav_idx_ = -1;
    int32_t nav_page_ = -1; // page where the last chapter jump landed; nav_idx_ is only trusted while it is shown
    std::size_t chapter_count_ = 0;
    bool in_reader_ = false, fullscreen_ = false, toc_visible_ = false, smoke_keys_sent_ = false, smoke_theme_ = false, in_settings_ = false;
    int resume_page_ = -1;
    uint32_t pane_version_ = 0;
    std::string smoke_open_, reading_id_;
    struct Soak {
        std::vector<std::string> ids;
        std::chrono::steady_clock::time_point t0, deadline;
        std::size_t next = 0;
        long long minutes = 0, logged = -1, books = 0, pages = 0;
        int book_pages = 0, last_page = -1;
        bool tray_visit = false;
        int tray_ticks = 0;
    } soak_;
    WINDOWPLACEMENT saved_wp_{};
    LONG_PTR saved_style_ = 0;
    HMENU saved_menu_ = nullptr;
    Tray tray_;
    ExternalOpen ext_;
    LaunchArgs launch_;
    std::uint64_t instance_key_ = 0;
    std::deque<std::wstring> pending_paths_;
    TtsState tts_state_ = TtsState::Unavailable;
    bool in_tray_ = false, asking_ = false;
    TtsBar tts_bar_;
    TtsPrefs tts_prefs_;
    std::unique_ptr<TtsEngines> tts_engines_;
    std::unique_ptr<TtsController> tts_;
    bool tts_prefs_loaded_ = false, tts_lang_pending_ = false, smoke_tts_ = false, smoke_tts_run_ = false, azure_dirty_ = false;
};

}
