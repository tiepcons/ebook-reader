#include "ExternalOpen.h"

#include "base/Log.h"
#include "lib/Metadata.h"

namespace eb {

bool ExternalOpen::start(HWND notify, std::filesystem::path file) noexcept
{
    try {
        {
            const std::scoped_lock lock(mu_);
            if (busy_)
                return false;
            busy_ = true;
        }
        thread_ = std::jthread([this, notify, f = std::move(file)]() mutable { run(notify, std::move(f)); });
        return true;
    } catch (...) {
        const std::scoped_lock lock(mu_);
        busy_ = false;
        return false;
    }
}

void ExternalOpen::run(HWND notify, std::filesystem::path file) noexcept
{
    ExternalMeta out{std::move(file), {}, false};
    try {
        if (auto m = read_book_meta(out.file); m) {
            out.meta = std::move(*m);
            out.ok = true;
        }
    } catch (...) {
        log::error("external open: metadata read threw");
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
