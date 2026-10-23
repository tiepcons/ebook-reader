#pragma once
#include "base/Handle.h"
#include "base/Paths.h"
#include "lib/Db.h"

#include <windows.h>

#include <chrono>

namespace eb {

struct FontTraits {
    using Raw = HFONT;
    static bool valid(HFONT f) noexcept { return f != nullptr; }
    static void close(HFONT f) noexcept { DeleteObject(f); }
};
using UniqueFont = Unique<FontTraits>;

inline constexpr UINT kMsgRescan = WM_APP + 1;

class MainWnd {
public:
    MainWnd(HINSTANCE inst, Db& db, const AppPaths& paths, bool smoke,
        std::chrono::steady_clock::time_point start) noexcept
        : inst_(inst), db_(db), paths_(paths), smoke_(smoke), start_(start)
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

    HINSTANCE inst_;
    Db& db_;
    const AppPaths& paths_;
    bool smoke_;
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
};

}
