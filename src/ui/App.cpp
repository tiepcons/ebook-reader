#include "App.h"

#include "MainWnd.h"
#include "SingleInstance.h"
#include "base/ComScope.h"
#include "base/CrashGuard.h"
#include "base/DllLoader.h"
#include "base/Log.h"
#include "base/Paths.h"
#include "base/Text.h"
#include "lib/Db.h"
#include "lib/Schema.h"
#include "eb_mupdf.h"

#include <commctrl.h>
#include <delayimp.h>
#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace eb {
namespace {

struct EngineScope {
    ~EngineScope() { eb_shutdown(); }
};

constexpr int kUiBusyTimeoutMs = 50;
bool g_quiet = false;

void show_fatal(const wchar_t* message) noexcept
{
    if (g_quiet)
        return;
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
    try {
        const auto msg = wide_to_utf8(message);
        const auto detail = err != nullptr ? wide_to_utf8(err->detail) : Result<std::string>{};
        log::error("startup failed: {} [code {}: {}]", msg.value_or(""), err != nullptr ? static_cast<int>(err->code) : -1,
            detail.value_or(""));
    } catch (...) { // NOLINT(bugprone-empty-catch): the dialog must always be reached
    }
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

struct ArgvFree {
    void operator()(wchar_t** p) const noexcept { LocalFree(static_cast<void*>(p)); }
};

bool has_flag(std::wstring_view flag) noexcept
{
    int argc = 0;
    const std::unique_ptr<wchar_t*, ArgvFree> argv(CommandLineToArgvW(GetCommandLineW(), &argc));
    for (int i = 1; argv && i < argc; ++i) {
        if (std::wstring_view(argv.get()[i]) == flag)
            return true;
    }
    return false;
}

LaunchArgs read_launch_args()
{
    int argc = 0;
    const std::unique_ptr<wchar_t*, ArgvFree> argv(CommandLineToArgvW(GetCommandLineW(), &argc));
    std::vector<std::wstring_view> args;
    for (int i = 1; argv && i < argc; ++i)
        args.emplace_back(argv.get()[i]);
    return parse_launch_args(args);
}

bool load_all_imports() noexcept
{
    __try {
        return SUCCEEDED(__HrLoadAllImportsForDll("eb_mupdf.dll"));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void register_font_dirs(const std::filesystem::path& exe_dir) noexcept
{
    try {
        std::vector<std::wstring> dirs{(exe_dir / L"fonts").wstring()};
        PWSTR system_fonts = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Fonts, 0, nullptr, &system_fonts)))
            dirs.emplace_back(system_fonts);
        CoTaskMemFree(system_fonts);
        std::vector<const wchar_t*> raw;
        for (const auto& d : dirs)
            raw.push_back(d.c_str());
        if (eb_set_font_dirs(raw.data(), static_cast<int32_t>(raw.size())) != EB_OK)
            log::error("font directories rejected: {}", eb_last_error());
    } catch (...) {
        log::error("font directories not registered");
    }
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

int run(HINSTANCE inst, int show)
{
    const auto start = std::chrono::steady_clock::now();
    const bool smoke = has_flag(L"--smoke");
    g_quiet = smoke;
    const ComScope com(COINIT_APARTMENTTHREADED);
    const auto exe_dir = exe_directory();
    if (exe_dir.empty())
        return fatal(L"Không xác định được thư mục ứng dụng.");
    LaunchArgs launch = read_launch_args();
    const auto key = instance_key(exe_dir);
    const SingleInstance single(key);
    if (!single.is_first())
        return single.forward(launch.path) ? 0 : 1;
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
    if (!load_all_imports())
        return fatal(L"Thư viện libs\\eb_mupdf.dll không đầy đủ hoặc bị hỏng. Hãy cài lại ứng dụng.");
    if (eb_init(nullptr) != EB_OK)
        return fatal(L"Không khởi tạo được bộ xử lý tài liệu.");
    const EngineScope engine;
    register_font_dirs(exe_dir);
    auto db = Db::open(paths->data / L"library.db", kUiBusyTimeoutMs);
    if (!db || !migrate(*db))
        return fatal(L"Không mở được cơ sở dữ liệu thư viện.", db ? nullptr : &db.error());
    const CrashLoopGuard guard(paths->data / L"pending");
    if (const auto s = guard.previous_suspect())
        log::error("previous run crashed during: {}", *s);
    MainWnd wnd(inst, *db, *paths, smoke, smoke && has_flag(L"--smoke-scan"), start);
    wnd.set_launch(std::move(launch), key);
    if (!wnd.create(show))
        return fatal(L"Không tạo được cửa sổ chính.");
    const int code = run_loop();
    return smoke ? 0 : code;
}

} // namespace

int run_app(HINSTANCE inst, int show) noexcept
{
    try {
        return run(inst, show);
    } catch (...) {
        show_fatal(L"Đã xảy ra lỗi không mong muốn khi khởi động.");
        return 1;
    }
}

}
