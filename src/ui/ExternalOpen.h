#pragma once
#include "UiMessages.h"
#include "lib/LibraryModel.h"

#include <windows.h>

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace eb {

struct ExternalJob {
    std::wstring raw_path;
    std::filesystem::path db_file, marker_file;
};

struct ExternalMeta {
    ExternalFile file;
    BookMeta meta;
    bool ok = false; // the file is a usable book; meta stays empty when it could not be read
};

// Does every file read for a book that is named from outside the library (path checks, hashing, metadata)
// on a worker thread, then posts WM_APP_EXTERNAL_META to the UI thread, which collects the answer with take().
class ExternalOpen {
public:
    ExternalOpen() = default;
    ExternalOpen(const ExternalOpen&) = delete;
    ExternalOpen& operator=(const ExternalOpen&) = delete;

    // False while an earlier request is still running.
    bool start(HWND notify, ExternalJob job) noexcept;
    std::optional<ExternalMeta> take() noexcept;

private:
    void run(HWND notify, const ExternalJob& job) noexcept;

    std::mutex mu_;
    std::optional<ExternalMeta> done_;
    bool busy_ = false;
    std::jthread thread_;
};

}
