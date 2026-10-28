#pragma once
#include "lib/Db.h"
#include "tts/LangDetect.h"

#include <array>
#include <cstdint>
#include <string>

namespace eb {

enum class TtsKind : uint8_t { WinRt, Piper, Azure };
inline constexpr std::size_t kTtsKinds = 3;
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

[[nodiscard]] TtsPrefs load_tts_prefs(Db& db);
bool save_tts_prefs(Db& db, const TtsPrefs& p);

}
