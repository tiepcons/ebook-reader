#include "MainWnd.h"

#include "Kv.h"
#include "base/Log.h"
#include "base/Text.h"
#include "lib/LibraryModel.h"
#include "resource.h"

#include <psapi.h>

namespace eb {
namespace {

constexpr UINT_PTR kTrimTimer = 30, kHoldTimer = 31, kSmokeTrayTimer = 32;
constexpr UINT kTrimDelayMs = 400, kSmokeTrayCheckMs = 600;
constexpr size_t kBytesPerKb = 1024;
constexpr const char* kHintKey = "ui.tray_hint_shown";
constexpr const char* kMinToTrayKey = "ui.min_to_tray";

std::wstring title_of(Db& db, const std::string& id)
{
    auto st = db.prepare("SELECT title FROM books WHERE id=?1");
    if (!st || !st->bind_text(1, id))
        return {};
    const auto step = st->step();
    if (!step || *step != Step::Row || st->is_null(0))
        return {};
    return utf8_to_wide(st->column_text(0)).value_or(std::wstring{});
}

int menu_id(CloseAction a) noexcept
{
    switch (a) {
    case CloseAction::Exit:
        return IDM_CLOSE_EXIT;
    case CloseAction::Tray:
        return IDM_CLOSE_TRAY;
    default:
        return IDM_CLOSE_ASK;
    }
}

CloseAction action_of(int id) noexcept
{
    return id == IDM_CLOSE_EXIT ? CloseAction::Exit : id == IDM_CLOSE_TRAY ? CloseAction::Tray : CloseAction::Ask;
}

}

bool MainWnd::on_tray_msg(UINT msg, WPARAM wp, LPARAM lp, LRESULT& res)
{
    res = 0;
    if (const UINT created = Tray::taskbar_created_message(); created != 0 && msg == created) {
        tray_.readd();
        return true;
    }
    switch (msg) {
    case WM_APP_TRAY:
        if (const TrayAction a = tray_action(lp); a == TrayAction::Restore)
            bring_to_front();
        else if (a == TrayAction::Menu)
            tray_.show_menu(wp);
        return true;
    case WM_COPYDATA:
        res = on_copydata(lp) ? TRUE : FALSE;
        return true;
    case WM_APP_OPEN_PATH:
        on_open_path();
        return true;
    case WM_APP_EXTERNAL_META:
        on_external_meta();
        return true;
    case WM_COMMAND:
        return on_tray_command(LOWORD(wp));
    case WM_TIMER:
        return on_tray_timer(wp);
    case WM_INITMENUPOPUP:
        CheckMenuRadioItem(reinterpret_cast<HMENU>(wp), IDM_CLOSE_ASK, IDM_CLOSE_TRAY, // NOLINT(performance-no-int-to-ptr): Win32 message ABI
            static_cast<UINT>(menu_id(load_close_action(db_))), MF_BYCOMMAND);
        return false;
    default:
        return false;
    }
}

bool MainWnd::on_tray_command(int id)
{
    const auto tts = [this](TtsCmd c) { PostMessageW(hwnd_, WM_APP_TTS_CMD, static_cast<WPARAM>(c), 0); };
    if (id >= IDM_CLOSE_ASK && id <= IDM_CLOSE_TRAY) {
        store_close_action(db_, action_of(id));
        return true;
    }
    switch (id) {
    case kIdTrayOpen:
        bring_to_front();
        return true;
    case kIdTrayPlayPause:
        tts(TtsCmd::PlayPause);
        return true;
    case kIdTrayNext:
        tts(TtsCmd::Next);
        return true;
    case kIdTrayStop:
        tts(TtsCmd::Stop);
        return true;
    case kIdTrayExit:
        exit_app();
        return true;
    default:
        return false;
    }
}

bool MainWnd::on_tray_timer(UINT_PTR id)
{
    if (id == kTrimTimer) {
        KillTimer(hwnd_, kTrimTimer);
        SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
        if (smoke_ && launch_.smoke_tray)
            SetTimer(hwnd_, kSmokeTrayTimer, kSmokeTrayCheckMs, nullptr);
    } else if (id == kSmokeTrayTimer) {
        KillTimer(hwnd_, kSmokeTrayTimer);
        smoke_tray_check();
    } else if (id == kHoldTimer) {
        KillTimer(hwnd_, kHoldTimer);
        exit_app();
    } else {
        return false;
    }
    return true;
}

void MainWnd::on_close_request()
{
    if (asking_)
        return;
    if (in_tray_) {
        exit_app();
        return;
    }
    CloseAction act = smoke_ ? (launch_.smoke_tray ? CloseAction::Tray : CloseAction::Exit) : load_close_action(db_);
    if (act == CloseAction::Ask) {
        asking_ = true;
        const CloseAnswer a = ask_close(hwnd_);
        asking_ = false;
        if (a.choice == CloseChoice::Cancel)
            return;
        if (const auto keep = remembered_action(a))
            store_close_action(db_, *keep);
        act = a.choice == CloseChoice::Exit ? CloseAction::Exit : CloseAction::Tray;
    }
    if (act == CloseAction::Tray)
        send_to_tray();
    else
        exit_app();
}

void MainWnd::on_minimized()
{
    if (const auto v = kv_get(db_, kMinToTrayKey); v && *v == "1")
        send_to_tray();
}

void MainWnd::exit_app()
{
    tray_.remove();
    close_book();
    if (!smoke_)
        save_state();
    DestroyWindow(hwnd_);
}

void MainWnd::send_to_tray()
{
    if (in_tray_)
        return;
    tray_.set_tooltip(in_reader_ ? title_of(db_, reading_id_) : std::wstring{});
    if (!tray_.add(hwnd_, dpi_)) {
        log::error("tray icon could not be added; minimizing instead");
        ShowWindow(hwnd_, SW_MINIMIZE);
        return;
    }
    in_tray_ = true;
    ShowWindow(hwnd_, SW_HIDE);
    if (in_reader_)
        PostMessageW(reader_.hwnd(), WM_APP_TO_TRAY, 0, 0);
    SetTimer(hwnd_, kTrimTimer, kTrimDelayMs, nullptr);
    if (!smoke_ && !kv_get(db_, kHintKey)) {
        tray_.balloon(L"Ebook Reader vẫn chạy ở khay hệ thống. Bấm vào biểu tượng để mở lại.");
        kv_set(db_, kHintKey, "1");
    }
}

void MainWnd::bring_to_front()
{
    if (in_tray_) {
        KillTimer(hwnd_, kTrimTimer);
        tray_.remove();
        in_tray_ = false;
    }
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd_);
}

bool MainWnd::on_copydata(LPARAM lp)
{
    const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lp); // NOLINT(performance-no-int-to-ptr): Win32 message ABI
    auto path = cds != nullptr ? decode_open_request(cds->dwData, cds->lpData, cds->cbData) : std::nullopt;
    if (!path) {
        log::error("copydata rejected");
        return false;
    }
    log::info("copydata open {}", wide_to_utf8(*path).value_or(std::string{}));
    bring_to_front();
    if (!path->empty()) {
        pending_path_ = std::move(*path);
        PostMessageW(hwnd_, WM_APP_OPEN_PATH, 0, 0);
    }
    return true;
}

void MainWnd::on_open_path()
{
    const std::wstring raw = std::move(pending_path_);
    pending_path_.clear();
    const auto file = resolve_external_book(raw);
    if (!file) {
        show_notice(L"Không mở được tệp: không tồn tại hoặc định dạng không được hỗ trợ.");
        return;
    }
    if (const auto id = find_book_by_path(db_, *file)) {
        open_external_id(*id);
    } else if (!ext_.start(hwnd_, *file)) {
        log::error("external open: a metadata read is already running");
    }
}

void MainWnd::on_external_meta()
{
    const auto done = ext_.take();
    if (!done)
        return;
    if (!done->ok) {
        show_notice(L"Không đọc được thông tin của sách này.");
        return;
    }
    const auto id = upsert_external(db_, done->file, done->meta);
    if (!id) {
        log::error("external open: cannot add the book to the library");
        show_notice(L"Không thêm được sách vào thư viện.");
        return;
    }
    refresh_tree();
    refresh_list();
    open_external_id(*id);
}

void MainWnd::open_external_id(const std::string& id)
{
    if (in_reader_)
        close_book();
    request_open(id);
}

void MainWnd::init_launch()
{
    SingleInstance::mark_window(hwnd_, instance_key_);
    if (smoke_ && launch_.hold_ms > 0)
        SetTimer(hwnd_, kHoldTimer, launch_.hold_ms, nullptr);
}

void MainWnd::after_first_paint_tray()
{
    if (smoke_ && launch_.smoke_tray)
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    if (!launch_.path.empty()) {
        pending_path_ = launch_.path;
        PostMessageW(hwnd_, WM_APP_OPEN_PATH, 0, 0);
    }
}

void MainWnd::shutdown_tray()
{
    for (const UINT_PTR t : {kTrimTimer, kHoldTimer, kSmokeTrayTimer})
        KillTimer(hwnd_, t);
    tray_.remove();
    SingleInstance::unmark_window(hwnd_);
}

void MainWnd::smoke_tray_check()
{
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    const bool mem = GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)) != FALSE;
    const bool hidden = !IsWindowVisible(hwnd_), icon = tray_.update();
    if (!mem || !hidden || !icon)
        log::error("smoke tray failed: hidden={} icon={} mem={}", hidden, icon, mem);
    log::info("tray-ws-kb {} tray-private-kb {}", pmc.WorkingSetSize / kBytesPerKb, pmc.PrivateUsage / kBytesPerKb);
    PostMessageW(hwnd_, WM_COMMAND, kIdTrayExit, 0);
}

}
