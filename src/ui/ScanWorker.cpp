#include "ScanWorker.h"

#include "base/CrashGuard.h"
#include "base/Log.h"
#include "lib/Db.h"

#include <chrono>
#include <cwctype>

namespace eb {
namespace {

constexpr auto kProgressGap = std::chrono::milliseconds(100);
constexpr int kPostTries = 3;
constexpr DWORD kPostRetryMs = 10;
constexpr int kBusyTimeoutMs = 2000;

bool post_retry(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) noexcept
{
    for (int i = 0; i < kPostTries; ++i) {
        if (PostMessageW(wnd, msg, wp, lp) != FALSE)
            return true;
        Sleep(kPostRetryMs);
    }
    return false;
}

bool library_changed(const ScanStats& s) noexcept
{
    return s.added + s.updated + s.moved + s.missing + s.bad > 0;
}

}

Result<BookMeta> stem_meta(const std::filesystem::path& file)
{
    try {
        BookMeta m;
        m.title = file.stem().wstring();
        m.fmt = file.extension().wstring();
        if (!m.fmt.empty())
            m.fmt.erase(0, 1);
        for (wchar_t& c : m.fmt)
            c = static_cast<wchar_t>(std::towlower(c));
        return m;
    } catch (...) {
        return fail(ErrorCode::NoMemory);
    }
}

void ScanWorker::run(HWND notify, const ScanJob& job) noexcept
{
    WPARAM result = 0;
    try {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        auto db = Db::open(job.db_file, kBusyTimeoutMs);
        if (db) {
            CrashLoopGuard guard(job.marker_file);
            auto last = std::chrono::steady_clock::now();
            const auto progress = [&](int done, int total) {
                const auto now = std::chrono::steady_clock::now();
                if (now - last < kProgressGap && done < total)
                    return;
                last = now;
                PostMessageW(notify, WM_APP_SCAN_PROGRESS, static_cast<WPARAM>(done), static_cast<LPARAM>(total));
            };
            const auto r = scan_library(*db, job.roots, job.meta, guard, cancel_, progress, job.exclude_dirs);
            if (r) {
                result = library_changed(*r) ? 3 : 1;
                log::info("scan: +{} ~{} moved {} missing {} bad {}", r->added, r->updated, r->moved, r->missing, r->bad);
            } else {
                log::error("scan failed (code {})", static_cast<int>(r.error().code));
            }
        } else {
            log::error("scan: cannot open database");
        }
    } catch (...) {
        log::error("scan: exception");
    }
    done_ = true;
    if (!post_retry(notify, WM_APP_SCAN_DONE, result, 0))
        log::error("scan: completion message could not be posted");
}

bool ScanWorker::start(HWND notify, ScanJob job) noexcept
{
    if (running_) {
        if (!done_)
            return false;
        finish();
    }
    try {
        cancel_ = false;
        done_ = false;
        thread_ = std::jthread([this, notify, j = std::move(job)] { run(notify, j); });
        running_ = true;
        return true;
    } catch (...) {
        log::error("scan: cannot start worker thread");
        return false;
    }
}

void ScanWorker::finish() noexcept
{
    if (thread_.joinable())
        thread_.join();
    running_ = false;
}

void ScanWorker::stop() noexcept
{
    cancel_ = true;
    finish();
}

}
