#include "MainWnd.h"

#include "ReaderSettings.h"
#include "base/Log.h"
#include "base/Text.h"

#include <shellapi.h>

#include <filesystem>
#include <memory>
#include <string_view>

namespace eb {
namespace {

constexpr UINT kSmokeWatchdogMs = 30000;
constexpr int kSmokeKeys = 3;

struct ArgvFree {
    void operator()(wchar_t** p) const noexcept { LocalFree(static_cast<void*>(p)); }
};

std::string smoke_open_name()
{
    int argc = 0;
    const std::unique_ptr<wchar_t*, ArgvFree> argv(CommandLineToArgvW(GetCommandLineW(), &argc));
    for (int i = 1; argv && i + 1 < argc; ++i) {
        if (std::wstring_view(argv.get()[i]) == L"--smoke-open")
            return wide_to_utf8(std::filesystem::path(argv.get()[i + 1]).filename().wstring()).value_or(std::string{});
    }
    return {};
}

bool has_arg(std::wstring_view flag)
{
    int argc = 0;
    const std::unique_ptr<wchar_t*, ArgvFree> argv(CommandLineToArgvW(GetCommandLineW(), &argc));
    for (int i = 1; argv && i < argc; ++i) {
        if (std::wstring_view(argv.get()[i]) == flag)
            return true;
    }
    return false;
}

}

void MainWnd::init_smoke_open()
{
    if (!smoke_)
        return;
    smoke_open_ = smoke_open_name();
    if (smoke_open_.empty())
        return;
    smoke_theme_ = has_arg(L"--smoke-theme");
    smoke_tts_ = has_arg(L"--smoke-tts");
    SetTimer(hwnd_, kSmokeTimer, kSmokeWatchdogMs, nullptr);
    PostMessageW(hwnd_, kMsgSmokeOpen, 0, 0);
}

void MainWnd::smoke_open_book()
{
    if (auto st = db_.prepare("SELECT id FROM books WHERE status=0 AND path LIKE ?1"); st && st->bind_text(1, "%\\" + smoke_open_)) {
        if (const auto step = st->step(); step && *step == Step::Row) {
            open_book(std::string(st->column_text(0)));
            if (in_reader_)
                return;
        }
    }
    log::error("smoke: cannot open {}", smoke_open_);
    PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

void MainWnd::smoke_step(int page)
{
    if (smoke_open_.empty())
        return;
    if (smoke_tts_ || smoke_tts_run_) {
        if (std::exchange(smoke_tts_, false)) {
            smoke_tts_run_ = true;
            tts_toggle();
        }
        return;
    }
    if (smoke_theme_) {
        smoke_theme_ = false;
        ReaderPrefs p = reader_.prefs();
        p.theme.id = ThemeId::Black;
        reader_.set_prefs(p, false);
        apply_chrome();
        return;
    }
    if (!smoke_keys_sent_ && page == 0) {
        smoke_keys_sent_ = true;
        smoke_marks();
        for (int i = 0; i < kSmokeKeys; ++i)
            PostMessageW(reader_.hwnd(), WM_KEYDOWN, VK_RIGHT, 0);
    } else if (smoke_keys_sent_ && page >= kSmokeKeys) {
        PostMessageW(reader_.hwnd(), WM_KEYDOWN, VK_ESCAPE, 0);
    }
}

void MainWnd::smoke_marks()
{
    const auto added = reader_.toggle_bookmark();
    const auto removed = reader_.toggle_bookmark();
    if (added.value_or(false) && !removed.value_or(true))
        log::info("smoke: bookmark added then removed");
    toggle_toc_pane();
}

}
