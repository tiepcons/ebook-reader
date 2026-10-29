#include "MainWnd.h"

#include "ReaderSettings.h"
#include "base/FileIo.h"
#include "base/Log.h"
#include "base/Text.h"

#include <psapi.h>
#include <shellapi.h>

#include <filesystem>
#include <format>
#include <memory>
#include <string_view>

namespace eb {
namespace {

constexpr UINT kSmokeWatchdogMs = 30000;
constexpr int kSmokeKeys = 3;
constexpr UINT kSoakTickMs = 200;
constexpr int kSoakPagesPerBook = 10, kSoakTrayEvery = 3, kSoakTrayTicks = 5;
constexpr long kSoakMaxMinutes = 10080;
constexpr size_t kBytesPerKb = 1024;

struct ArgvFree {
    void operator()(wchar_t** p) const noexcept { LocalFree(static_cast<void*>(p)); }
};

std::wstring arg_after(std::wstring_view flag)
{
    int argc = 0;
    const std::unique_ptr<wchar_t*, ArgvFree> argv(CommandLineToArgvW(GetCommandLineW(), &argc));
    for (int i = 1; argv && i + 1 < argc; ++i) {
        if (std::wstring_view(argv.get()[i]) == flag)
            return argv.get()[i + 1];
    }
    return {};
}

std::string smoke_open_name()
{
    const std::wstring v = arg_after(L"--smoke-open");
    return v.empty() ? std::string{} : wide_to_utf8(std::filesystem::path(v).filename().wstring()).value_or(std::string{});
}

long parse_minutes(const std::wstring& v)
{
    wchar_t* end = nullptr;
    const long m = v.empty() ? 0 : std::wcstol(v.c_str(), &end, 10);
    return (m >= 1 && m <= kSoakMaxMinutes && *end == L'\0') ? m : 0;
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
    init_soak();
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
    if (soak_.minutes != 0) {
        if (soak_.last_page >= 0 && page != soak_.last_page)
            ++soak_.pages;
        soak_.last_page = page;
    }
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

void MainWnd::init_soak()
{
    const long minutes = parse_minutes(arg_after(L"--smoke-soak"));
    if (minutes == 0)
        return;
    if (auto st = db_.prepare("SELECT id FROM books WHERE status=0 ORDER BY path")) {
        for (auto step = st->step(); step && *step == Step::Row; step = st->step())
            soak_.ids.emplace_back(st->column_text(0));
    }
    if (soak_.ids.empty()) {
        log::error("soak: the library has no readable book");
        return;
    }
    soak_.minutes = minutes;
    soak_.t0 = std::chrono::steady_clock::now();
    soak_.deadline = soak_.t0 + std::chrono::minutes(minutes);
    SetTimer(hwnd_, kSoakTimer, kSoakTickMs, nullptr);
}

void MainWnd::soak_log()
{
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    DWORD handles = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    const auto t = std::chrono::duration_cast<std::chrono::minutes>(std::chrono::steady_clock::now() - soak_.t0).count();
    const std::string line = std::format("soak: t={} books={} pages={} ws={} private={} handles={} gdi={}\n", t, soak_.books,
        soak_.pages, pmc.WorkingSetSize / kBytesPerKb, pmc.PrivateUsage / kBytesPerKb, handles,
        GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS));
    if (!append_file(paths_.logs / L"soak.log", line))
        log::error("soak: cannot append to soak.log");
}

void MainWnd::soak_tick()
{
    const auto now = std::chrono::steady_clock::now();
    const long long minute = std::chrono::duration_cast<std::chrono::minutes>(now - soak_.t0).count();
    if (minute > soak_.logged || now >= soak_.deadline) {
        soak_.logged = minute;
        soak_log();
    }
    if (now >= soak_.deadline) {
        KillTimer(hwnd_, kSoakTimer);
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        return;
    }
    if (in_tray_) {
        if (++soak_.tray_ticks >= kSoakTrayTicks) { // stay past kTrimTimer so the tray trim runs
            soak_.tray_ticks = 0;
            bring_to_front();
        }
        return;
    }
    if (!in_reader_) {
        open_book(soak_.ids[soak_.next++ % soak_.ids.size()]);
        soak_.book_pages = 0;
        soak_.last_page = -1;
        soak_.tray_visit = false;
        soak_.books += in_reader_ ? 1 : 0;
    } else if (soak_.book_pages < kSoakPagesPerBook) {
        PostMessageW(reader_.hwnd(), WM_KEYDOWN, VK_RIGHT, 0);
        ++soak_.book_pages;
    } else if (soak_.books % kSoakTrayEvery == 0 && !soak_.tray_visit) {
        soak_.tray_visit = true;
        send_to_tray();
    } else {
        close_book();
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
