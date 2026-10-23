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
    std::filesystem::path path_;
};

}
