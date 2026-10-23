#include "Paths.h"

#include "Handle.h"
#include "WinError.h"

#include <array>
#include <new>
#include <string>
#include <system_error>

namespace eb {
namespace fs = std::filesystem;
namespace {

Result<fs::path> default_local_root()
{
    std::array<wchar_t, MAX_PATH> buf{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size())
        return fail(ErrorCode::NotFound, L"LOCALAPPDATA not set");
    return fs::path(std::wstring(buf.data(), n)) / L"EbookReader";
}

bool make_dir(const fs::path& p) noexcept
{
    try {
        std::error_code ec;
        fs::create_directories(p, ec);
        return !ec;
    } catch (...) {
        return false;
    }
}

}

bool dir_is_writable(const fs::path& dir) noexcept
{
    try {
        const fs::path probe = dir / (L".eb_probe_" + std::to_wstring(GetCurrentProcessId()));
        const UniqueHandle h(CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
        return h.valid();
    } catch (...) {
        return false;
    }
}

Result<AppPaths> resolve_paths(const fs::path& exe_dir)
{
    auto local = default_local_root();
    if (!local)
        return std::unexpected(local.error());
    return resolve_paths(exe_dir, *local, &dir_is_writable);
}

Result<AppPaths> resolve_paths(const fs::path& exe_dir, const fs::path& local_root, WritableProbe probe)
{
    try {
        AppPaths p;
        p.exe_dir = exe_dir;
        p.ebooks = exe_dir / L"ebooks";
        p.libs = exe_dir / L"libs";
        p.fonts = exe_dir / L"fonts";
        p.data = probe(exe_dir) ? exe_dir / L"data" : local_root;
        p.covers = p.data / L"covers";
        p.tts_cache = p.data / L"tts";
        p.crash = p.data / L"crash";
        p.logs = p.data / L"logs";
        p.voices = p.data / L"voices";
        for (const fs::path* d : {&p.ebooks, &p.fonts, &p.libs})
            make_dir(*d);
        for (const fs::path* d : {&p.data, &p.covers, &p.tts_cache, &p.crash, &p.logs, &p.voices}) {
            if (!make_dir(*d))
                return fail(ErrorCode::Io, L"cannot create " + d->wstring());
        }
        return p;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::NoMemory);
    } catch (...) {
        return fail(ErrorCode::Internal);
    }
}

}
