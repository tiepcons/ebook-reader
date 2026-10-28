#include "MainWnd.h"

#include "Kv.h"
#include "base/Log.h"
#include "base/SecretStore.h"
#include "base/Text.h"
#include "lib/AzureUsage.h"

#include <ctime>

namespace eb {
namespace {

int64_t now_unix() noexcept
{
    return static_cast<int64_t>(std::time(nullptr));
}

}

AzureView MainWnd::azure_view()
{
    AzureView v;
    v.region = kv_get(db_, kAzureRegionKey).value_or(std::string{});
    v.tier = parse_tier(kv_get(db_, kAzureTierKey).value_or(std::string{}));
    v.has_key = secret_get(kAzureSecretTarget).has_value();
    if (const auto s = AzureUsage(db_).status(now_unix())) {
        v.usage = *s;
        v.usage_ok = true;
    }
    return v;
}

bool MainWnd::azure_save(const AzureEdits& e)
{
    const auto old_region = kv_get(db_, kAzureRegionKey).value_or(std::string{});
    bool ok = kv_set(db_, kAzureRegionKey, e.region) && kv_set(db_, kAzureTierKey, tier_name(e.tier))
        && kv_set(db_, kAzureQuotaKey, std::to_string(e.quota));
    if (ok && e.used)
        ok = AzureUsage(db_).calibrate(now_unix(), *e.used).has_value();
    azure_dirty_ = azure_dirty_ || e.key_changed || old_region != e.region;
    return ok;
}

bool MainWnd::azure_reset_used()
{
    return AzureUsage(db_).reset_month(now_unix()).has_value();
}

void MainWnd::refresh_azure_quota()
{
    std::wstring bar, tray;
    if (tts_engines_ && tts_engines_->azure) {
        if (const auto s = AzureUsage(db_).status(now_unix())) {
            bar = bar_quota_text(*s);
            tray = tray_quota_text(*s);
        }
    }
    tts_bar_.set_quota(bar);
    tray_.set_quota_text(tray);
}

void MainWnd::on_azure_warn(Warn level)
{
    const std::wstring text = warn_text(level);
    if (text.empty())
        return;
    notice(text.c_str());
    if (tray_.present())
        tray_.balloon(text.c_str());
    refresh_azure_quota();
}

void MainWnd::rebuild_azure_engine()
{
    if (!tts_engines_)
        return;
    if (tts_)
        tts_->stop();
    tts_.reset();
    reload_azure_engine(*tts_engines_, paths_, db_, hwnd_);
    try {
        tts_ = std::make_unique<TtsController>(static_cast<TtsHost&>(*this), tts_engines_->pointers());
        tts_->set_prefs(tts_prefs_);
    } catch (...) {
        log::error("tts: cannot restart the controller");
        tts_.reset();
        tts_engines_.reset();
    }
}

}
