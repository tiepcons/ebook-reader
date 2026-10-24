#pragma once
#include "UiMessages.h"
#include "lib/Scanner.h"

#include <windows.h>

#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace eb {

struct ExternalMeta {
    std::filesystem::path file;
    BookMeta meta;
    bool ok = false;
};

// Reads the metadata of a book that is not in the library on a worker thread, then posts
// WM_APP_EXTERNAL_META to the UI thread, which collects the answer with take().
class ExternalOpen {
public:
    ExternalOpen() = default;
    ExternalOpen(const ExternalOpen&) = delete;
    ExternalOpen& operator=(const ExternalOpen&) = delete;

    // False while an earlier request is still running.
    bool start(HWND notify, std::filesystem::path file) noexcept;
    std::optional<ExternalMeta> take() noexcept;

private:
    void run(HWND notify, std::filesystem::path file) noexcept;

    std::mutex mu_;
    std::optional<ExternalMeta> done_;
    bool busy_ = false;
    std::jthread thread_;
};

}
