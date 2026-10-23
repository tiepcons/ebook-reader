#include "App.h"

#include "MainWnd.h"
#include "base/CrashGuard.h"
#include "base/DllLoader.h"
#include "base/Log.h"
#include "base/Paths.h"
#include "base/Text.h"
#include "lib/Db.h"
#include "lib/Schema.h"
#include "eb_mupdf.h"

#include <commctrl.h>
#include <objbase.h>

#include <chrono>
#include <filesystem>
#include <string>

namespace eb {
namespace {

struct ComScope {
    bool ok;
    ComScope() noexcept : ok(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {}
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    ~ComScope()
    {
        if (ok)
            CoUninitialize();
    }
};

struct EngineScope {
    ~EngineScope() { eb_shutdown(); }
};

void show_fatal(const wchar_t* message) noexcept
{
    TASKDIALOGCONFIG cfg{};
    cfg.cbSize = sizeof(cfg);
    cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    cfg.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    cfg.pszWindowTitle = L"Ebook Reader";
    cfg.pszMainIcon = TD_ERROR_ICON;
    cfg.pszContent = message;
    if (FAILED(TaskDialogIndirect(&cfg, nullptr, nullptr, nullptr)))
        MessageBoxW(nullptr, message, L"Ebook Reader", MB_OK | MB_ICONERROR);
}

int fatal(const wchar_t* message, const Error* err = nullptr) noexcept
{
    const auto msg = wide_to_utf8(message);
    const auto detail = err != nullptr ? wide_to_utf8(err->detail) : Result<std::string>{};
    log::error("startup failed: {} [code {}: {}]", msg.value_or(""), err != nullptr ? static_cast<int>(err->code) : -1,
        detail.value_or(""));
    show_fatal(message);
    return 1;
}

std::filesystem::path exe_directory()
{
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0)
            return {};
        if (n < buf.size()) {
            buf.resize(n);
            return std::filesystem::path(buf).parent_path();
        }
        if (buf.size() > 32768)
            return {};
        buf.resize(buf.size() * 2);
    }
}

bool has_flag(const wchar_t* cmdline, const wchar_t* flag) noexcept
{
    return cmdline != nullptr && std::wstring_view(cmdline).find(flag) != std::wstring_view::npos;
}

int run_loop() noexcept
{
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

int run(HINSTANCE inst, const wchar_t* cmdline, int show)
{
    const auto start = std::chrono::steady_clock::now();
    const bool smoke = has_flag(cmdline, L"--smoke");
    const ComScope com;
    const auto exe_dir = exe_directory();
    if (exe_dir.empty())
        return fatal(L"Không xác định được thư mục ứng dụng.");
    auto paths = resolve_paths(exe_dir);
    if (!paths)
        return fatal(L"Không tạo được thư mục dữ liệu. Hãy kiểm tra quyền ghi.", &paths.error());
    log::init(paths->logs / L"app.log");
    struct LogScope {
        ~LogScope() { log::shutdown(); }
    } const log_scope;
    install_crash_handler(paths->crash);
    if (auto r = secure_dll_search(paths->libs); !r)
        return fatal(L"Không thể bảo vệ đường dẫn nạp thư viện.", &r.error());
    auto mod = load_checked(paths->libs / L"eb_mupdf.dll", "eb_abi_version", EB_ABI_VERSION);
    if (!mod)
        return fatal(L"Thiếu thư viện libs\\eb_mupdf.dll hoặc sai phiên bản. Hãy cài lại ứng dụng.", &mod.error());
    if (eb_init(nullptr) != EB_OK)
        return fatal(L"Không khởi tạo được bộ xử lý tài liệu.");
    const EngineScope engine;
    auto db = Db::open(paths->data / L"library.db");
    if (!db || !migrate(*db))
        return fatal(L"Không mở được cơ sở dữ liệu thư viện.", db ? nullptr : &db.error());
    const CrashLoopGuard guard(paths->data / L"pending");
    if (const auto s = guard.previous_suspect())
        log::error("previous run crashed during: {}", *s);
    MainWnd wnd(inst, *db, *paths, smoke, start);
    if (!wnd.create(show))
        return fatal(L"Không tạo được cửa sổ chính.");
    const int code = run_loop();
    return smoke ? 0 : code;
}

} // namespace

int run_app(HINSTANCE inst, const wchar_t* cmdline, int show) noexcept
{
    try {
        return run(inst, cmdline, show);
    } catch (...) {
        show_fatal(L"Đã xảy ra lỗi không mong muốn khi khởi động.");
        return 1;
    }
}

}
