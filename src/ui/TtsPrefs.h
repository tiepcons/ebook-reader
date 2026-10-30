#pragma once
#include "lib/Db.h"
#include "tts/LangDetect.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eb {

enum class TtsKind : uint8_t { WinRt, Piper, Azure, VieNeu };
inline constexpr std::size_t kTtsKinds = 4;
inline constexpr double kMinTtsRate = 0.5, kMaxTtsRate = 2.0;
inline constexpr std::size_t kMaxVoiceIdChars = 256;

struct TtsPrefs {
    TtsKind engine = TtsKind::WinRt;
    double rate = 1.0;
    std::array<std::array<std::wstring, 2>, kTtsKinds> voice; // [engine][Lang]; empty = first voice of the language
    friend bool operator==(const TtsPrefs&, const TtsPrefs&) = default;
};

[[nodiscard]] constexpr std::size_t kind_index(TtsKind k) noexcept { return static_cast<std::size_t>(k); }
[[nodiscard]] double clamp_tts_rate(double rate) noexcept;
[[nodiscard]] std::string_view tts_kind_name(TtsKind k) noexcept;
// Position of `wanted` among the engines offered right now. Nullopt when it is not offered (VieNeu is hidden until its
// files are verified): the dialog must then keep the saved choice instead of replacing it with the first engine.
[[nodiscard]] std::optional<std::size_t> engine_index(const std::vector<TtsKind>& offered, TtsKind wanted) noexcept;

[[nodiscard]] TtsPrefs load_tts_prefs(Db& db);
bool save_tts_prefs(Db& db, const TtsPrefs& p);

}
