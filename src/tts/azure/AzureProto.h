#pragma once
#include "tts/TtsEngine.h"

#include <string>
#include <string_view>
#include <vector>

namespace eb {

inline constexpr std::string_view kAzureDefaultVoice = "vi-VN-HoaiMyNeural";
inline constexpr size_t kAzureMaxVoices = 4096;

// Lowercase letters and digits, 3..32 characters: the region becomes a host-name label.
bool valid_azure_region(std::string_view region) noexcept;
// <language>-<REGION>-<name>, ASCII letters, digits and '-', at most 64 characters.
bool valid_azure_voice(std::string_view voice) noexcept;
// rate 0.5..3.0 (NaN counts as 1.0) as a relative percentage: 1.0 -> 0, 0.5 -> -50, 3.0 -> +200.
int rate_percent(double rate) noexcept;
// Escapes & < > " ' and replaces control characters and ill-formed UTF-8 so the result is valid XML 1.0 text.
std::string xml_escape(std::string_view utf8);
// Everything Azure bills for a request: the <prosody> element inside <voice>.
std::string ssml_voice_body(std::string_view utf8, int rate_pct);
// Empty when the voice is invalid.
std::string build_ssml(std::string_view utf8, std::string_view voice, int rate_pct);
// Keeps vi-VN, en-* and the voice with id == keep. Voice::id is ShortName, name is LocalName, lang is Locale.
std::vector<Voice> parse_voices(std::string_view json, std::wstring_view keep);

}
