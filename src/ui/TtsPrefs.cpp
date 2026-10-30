#include "TtsPrefs.h"

#include "Kv.h"
#include "base/Text.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <string_view>

namespace eb {
namespace {

constexpr std::string_view kKeyEngine = "tts.engine", kKeyRatePct = "tts.rate_pct";
constexpr std::array<std::string_view, kTtsKinds> kKindNames{"winrt", "piper", "azure", "vieneu"};
constexpr std::array<std::string_view, 2> kLangNames{"en", "vi"};

std::string voice_key(std::size_t kind, std::size_t lang)
{
    return std::format("tts.voice.{}.{}", kKindNames[kind], kLangNames[lang]);
}

}

double clamp_tts_rate(double rate) noexcept
{
    return std::isfinite(rate) ? std::clamp(rate, kMinTtsRate, kMaxTtsRate) : 1.0;
}

std::string_view tts_kind_name(TtsKind k) noexcept
{
    return kKindNames[kind_index(k)];
}

std::optional<std::size_t> engine_index(const std::vector<TtsKind>& offered, TtsKind wanted) noexcept
{
    const auto it = std::ranges::find(offered, wanted);
    return it == offered.end() ? std::nullopt : std::optional<std::size_t>(static_cast<std::size_t>(it - offered.begin()));
}

TtsPrefs load_tts_prefs(Db& db)
{
    TtsPrefs p;
    if (const auto e = kv_get(db, kKeyEngine)) {
        const auto it = std::ranges::find(kKindNames, *e);
        if (it != kKindNames.end())
            p.engine = static_cast<TtsKind>(it - kKindNames.begin());
    }
    if (const auto r = kv_get(db, kKeyRatePct)) {
        int pct = 0;
        if (std::from_chars(r->data(), r->data() + r->size(), pct).ec == std::errc{})
            p.rate = clamp_tts_rate(pct / 100.0);
    }
    for (std::size_t k = 0; k < kTtsKinds; ++k) {
        for (std::size_t l = 0; l < kLangNames.size(); ++l) {
            const auto v = kv_get(db, voice_key(k, l));
            if (v && v->size() <= kMaxVoiceIdChars * 4)
                p.voice[k][l] = utf8_to_wide(*v).value_or(std::wstring{});
            if (p.voice[k][l].size() > kMaxVoiceIdChars)
                p.voice[k][l].clear();
        }
    }
    return p;
}

bool save_tts_prefs(Db& db, const TtsPrefs& p)
{
    bool ok = kv_set(db, kKeyEngine, kKindNames[kind_index(p.engine)])
        && kv_set(db, kKeyRatePct, std::to_string(std::lround(clamp_tts_rate(p.rate) * 100.0)));
    for (std::size_t k = 0; k < kTtsKinds; ++k) {
        for (std::size_t l = 0; l < kLangNames.size(); ++l)
            ok = kv_set(db, voice_key(k, l), wide_to_utf8(p.voice[k][l]).value_or(std::string{})) && ok;
    }
    return ok;
}

}
