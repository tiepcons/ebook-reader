#pragma once
#include <windows.h>

#include <filesystem>
#include <string>
#include <system_error>

namespace ebtest {

class TempDir {
public:
    TempDir()
    {
        static int counter = 0;
        path_ = std::filesystem::temp_directory_path()
            / (L"ebtest_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(++counter));
        std::filesystem::create_directories(path_);
        path_ = long_path(path_);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    const std::filesystem::path& path() const { return path_; }

private:
    // GetTempPath may return an 8.3 short form (CI runners); the product stores long paths.
    static std::filesystem::path long_path(const std::filesystem::path& p)
    {
        std::wstring buf(MAX_PATH, L'\0');
        DWORD n = GetLongPathNameW(p.c_str(), buf.data(), static_cast<DWORD>(buf.size()));
        if (n > buf.size()) {
            buf.resize(n);
            n = GetLongPathNameW(p.c_str(), buf.data(), static_cast<DWORD>(buf.size()));
        }
        if (n == 0 || n >= buf.size())
            return p;
        buf.resize(n);
        return buf;
    }

    std::filesystem::path path_;
};

}
