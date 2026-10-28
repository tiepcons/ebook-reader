#pragma once
#include "FontPrefs.h"
#include "ReaderCanvas.h"
#include "ReaderLogic.h"
#include "ReaderWorker.h"
#include "UiMessages.h"
#include "lib/Db.h"
#include "lib/ProgressStore.h"

#include <windows.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace eb {

// Child window that shows one book page at a time. The UI thread only uploads bitmaps and routes input;
// all MuPDF work happens in the RenderWorker. Reports to the parent with WM_APP_READER_EVENT.
class ReaderView {
public:
    explicit ReaderView(Db& db) : db_(db), store_(db), cache_(kBitmapCacheBytes) {}
    ReaderView(const ReaderView&) = delete;
    ReaderView& operator=(const ReaderView&) = delete;
    ~ReaderView();

    bool create(HINSTANCE inst, HWND parent) noexcept;
    [[nodiscard]] HWND hwnd() const noexcept { return hwnd_; }

    bool open(OpenJob job);
    void close() noexcept;
    void flush() noexcept;
    void tick() noexcept;
    void go_to_start();
    void go_to_mark(uint64_t mark);
    void go_to_percent(int percent);

    [[nodiscard]] bool is_open() const noexcept { return worker_ != nullptr; }
    [[nodiscard]] std::wstring status_text() const;
    [[nodiscard]] const std::wstring& error_text() const noexcept { return error_; }
    // Percent of the saved position when a book opened somewhere after its start, once; -1 otherwise.
    [[nodiscard]] int take_resume_percent() noexcept;

    [[nodiscard]] int percent() const noexcept;
    [[nodiscard]] const std::vector<TocItem>& toc() const noexcept { return toc_; }
    [[nodiscard]] std::vector<Bookmark> bookmarks() const;
    // Adds a bookmark on the shown page or removes the ones on it: true = added, false = removed, nullopt = failed.
    [[nodiscard]] std::optional<bool> toggle_bookmark();
    bool remove_bookmark(int64_t id);
    // Changes whenever the outline arrives or the bookmarks change.
    [[nodiscard]] uint32_t content_version() const noexcept { return content_version_; }

    [[nodiscard]] const ReaderPrefs& prefs() const noexcept { return prefs_; }
    [[nodiscard]] ThemeColors colors() const noexcept { return resolve_theme(prefs_.theme); }
    // Colour for the sentence the text-to-speech engine is reading.
    [[nodiscard]] uint32_t highlight_color() const noexcept { return colors().highlight; }
    [[nodiscard]] int32_t page() const noexcept { return page_; }
    [[nodiscard]] int32_t page_count() const noexcept { return count_; }
    // Text-to-speech support. The text of a page arrives with ReaderEvent::Text (lParam = page).
    void request_text(int32_t page);
    [[nodiscard]] std::shared_ptr<const PageText> text_of(int32_t page) const;
    void turn_to(int32_t page) { go_to(page); }
    // Marks the sentence [begin, end) of a page's text; painted while that page is shown. Returns the number of rectangles.
    std::size_t set_highlight(int32_t page, uint32_t begin, uint32_t end);
    void clear_highlight();
    // Records the reading position inside a page as tts_char, together with the page bookmark.
    void save_tts_position(int32_t page, uint32_t offset);
    // Sentence offset to continue from on the shown page, -1 when none was saved.
    [[nodiscard]] int64_t resume_tts_char() const noexcept;

    // Applies theme and font settings at once: the cache is dropped and the current page is requested first.
    void set_prefs(const ReaderPrefs& p, bool persist);

private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) noexcept;
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    bool on_mouse(UINT msg, WPARAM wp, LPARAM lp);
    void on_paint();
    void on_results();
    void store_result(RenderResult& r);
    void show_primary(const RenderResult& r);
    void fail_open(const RenderResult& r);
    void record_progress(const RenderResult& r) noexcept;
    void store_text(const RenderResult& r);
    [[nodiscard]] std::vector<D2D1_RECT_F> highlight_rects(int origin_x, int origin_y) const;
    void check_epoch();
    void set_shown(BitmapRef b);
    void rebuild_style();
    void bump_font_size(int direction);

    [[nodiscard]] ViewParams current_view() const;
    void send_want();
    void apply_view();
    void go_to(int32_t page, bool from_end = false);
    void on_action(NavAction a);
    void zoom(int direction);
    void on_wheel(int delta, bool ctrl);
    void wheel_step(int direction);
    void pan_by(int dx, int dy);
    void notify(ReaderEvent e) const noexcept;
    [[nodiscard]] SIZE content_size() const noexcept;
    [[nodiscard]] SIZE client_size() const noexcept;

    HWND hwnd_ = nullptr;
    HWND parent_ = nullptr;
    Db& db_;
    ProgressStore store_;
    std::optional<ProgressSaver> saver_;
    std::unique_ptr<RenderWorker> worker_;
    ReaderCanvas canvas_;
    ByteLru<BitmapRef> cache_;
    BitmapRef shown_;
    ViewParams view_;
    ZoomState zoom_;
    ReaderPrefs prefs_;
    std::shared_ptr<const ReaderStyle> style_;
    std::string fmt_, book_id_;
    std::vector<TocItem> toc_;
    uint32_t content_version_ = 0;
    uint64_t end_mark_ = UINT64_MAX;
    bool prefs_loaded_ = false;
    uint32_t view_id_ = 0;
    uint32_t epoch_ = 0;
    uint64_t want_gen_ = 0;
    uint64_t anchor_ = kNoAnchor;
    uint64_t mark_ = kNoAnchor;
    int32_t page_ = 0, count_ = 0;
    float scale_ = 0;
    bool reflowable_ = false, awaiting_first_ = false;
    double saved_percent_ = 0;
    int resume_percent_ = -1;
    int pan_x_ = 0, pan_y_ = 0;
    int wheel_acc_ = 0;
    bool dragging_ = false, moved_ = false;
    POINT drag_start_{}, drag_last_{};
    int64_t last_flush_ms_ = 0;
    std::wstring chapter_, note_, error_;
    std::vector<std::shared_ptr<const PageText>> texts_;
    std::vector<TextBox> highlight_;
    int32_t highlight_page_ = -1, tts_page_ = -1;
    int64_t tts_char_ = -1, saved_tts_char_ = -1;
    uint64_t saved_mark_ = kNoAnchor;
};

}
