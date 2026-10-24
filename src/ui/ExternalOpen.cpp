#include "ExternalOpen.h"

#include "base/CrashGuard.h"
#include "base/Log.h"
#include "lib/Metadata.h"

namespace eb {
namespace {

constexpr int kBusyTimeoutMs = 2000;

ExternalMeta inspect(const ExternalJob& job)
{
    ExternalMeta out;
    auto db = Db::open(job.db_file, kBusyTimeoutMs);
    if (!db)
        return out;
    auto file = inspect_external_book(*db, job.raw_path);
    if (!file)
        return out;
    out.file = std::move(*file);
    out.ok = true;
    CrashLoopGuard guard(job.marker_file);
    if (guard.previous_suspect() == out.file.id) {
        log::error("external open: skipping a book that crashed the metadata reader");
        return out;
    }
    const CrashLoopGuard::Scope scope(guard, "external", out.file.id);
    if (auto m = read_book_meta(out.file.path); m)
        out.meta = std::move(*m);
    return out;
}

}

bool ExternalOpen::start(HWND notify, ExternalJob job) noexcept
{
    try {
        {
            const std::scoped_lock lock(mu_);
            if (busy_)
                return false;
            busy_ = true;
        }
        thread_ = std::jthread([this, notify, j = std::move(job)]() { run(notify, j); });
        return true;
    } catch (...) {
        const std::scoped_lock lock(mu_);
        busy_ = false;
        return false;
    }
}

void ExternalOpen::run(HWND notify, const ExternalJob& job) noexcept
{
    ExternalMeta out;
    try {
        out = inspect(job);
    } catch (...) {
        log::error("external open: worker threw");
    }
    {
        const std::scoped_lock lock(mu_);
        done_ = std::move(out);
        busy_ = false;
    }
    PostMessageW(notify, WM_APP_EXTERNAL_META, 0, 0);
}

std::optional<ExternalMeta> ExternalOpen::take() noexcept
{
    const std::scoped_lock lock(mu_);
    return std::exchange(done_, std::nullopt);
}

}
