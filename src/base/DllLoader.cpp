#include "DllLoader.h"

#include "WinError.h"

namespace eb {

Result<void> secure_dll_search(const std::filesystem::path& libs_dir)
{
    if (!libs_dir.is_absolute())
        return fail(ErrorCode::Access, L"dll directory must be absolute");
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS))
        return fail_win32(GetLastError(), L"SetDefaultDllDirectories");
    if (AddDllDirectory(libs_dir.c_str()) == nullptr)
        return fail_win32(GetLastError(), L"AddDllDirectory");
    return {};
}

Result<UniqueModule> load_checked(const std::filesystem::path& abs_dll, const char* version_export, int expected)
{
    if (version_export == nullptr || !abs_dll.is_absolute())
        return fail(ErrorCode::Access, L"dll path must be absolute and export name set");
    UniqueModule mod(LoadLibraryExW(abs_dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS));
    if (!mod)
        return fail_win32(GetLastError(), L"LoadLibraryEx");
    using VersionFn = int(__cdecl*)();
    const auto fn = reinterpret_cast<VersionFn>(GetProcAddress(mod.get(), version_export));
    if (fn == nullptr)
        return fail(ErrorCode::Abi, L"version export missing");
    if (fn() != expected)
        return fail(ErrorCode::Abi, L"abi version mismatch");
    return mod;
}

}
