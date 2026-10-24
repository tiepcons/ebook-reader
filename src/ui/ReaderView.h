#pragma once
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

namespace eb {

// Child window that shows one book page at a time. The UI thread only uploads bitmaps and routes input;
// all MuPDF work happens in the RenderWorker. Reports to the parent with WM_APP_READER_EVENT.
class ReaderView {
public:
    explicit ReaderView(Db& db) noexcept : store_(db), cache_(kBitmapCacheBytes) {}
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

    [[nodiscard]] bool is_open() const noexcept { return worker_ != nullptr; }
    [[nodiscard]] std::wstring status_text() const;
    [[nodiscard]] const std::wstring& error_text() const noexcept { return error_; }
    // Percent of the saved position when a book opened somewhere after its start, once; -1 otherwise.
    [[nodiscard]] int take_resume_percent() noexcept;

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
    void check_epoch();
    void set_shown(BitmapRef b);

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
    ProgressStore store_;
    std::optional<ProgressSaver> saver_;
    std::unique_ptr<RenderWorker> worker_;
    ReaderCanvas canvas_;
    ByteLru<BitmapRef> cache_;
    BitmapRef shown_;
    ViewParams view_;
    ZoomState zoom_;
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
};

}
