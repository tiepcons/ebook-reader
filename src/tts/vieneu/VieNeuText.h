#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Pure helpers of the VieNeu engine: what is written to the batch file, how headings are split and how the WAVs that
// audiocpp_cli returns are stitched together.
namespace eb::vieneu {

inline constexpr uint32_t kHeadingPauseMs = 300;
inline constexpr uint32_t kLeadSilenceMs = 300;
inline constexpr size_t kMaxWavBytes = size_t{96} * 1024 * 1024;

struct Heading {
    std::string head; // "Chương một." (the colon becomes a full stop)
    std::string rest;
};

// A short leading heading ("Chương một: Lâm Phàm ...", at most 5 words before the first colon, colon followed by a
// space and more text) is read as its own utterance; the model otherwise glues the heading to the sentence.
std::optional<Heading> split_heading(std::string_view utf8);

// One request line for --batch-text-file: control characters, line separators and invalid UTF-8 become spaces, runs
// of spaces collapse, the result is trimmed. Empty when nothing speakable is left.
std::string batch_line(std::string_view utf8);
// The lines (each from batch_line, none empty) joined with LF, UTF-8 without a byte order mark.
std::string batch_file(const std::vector<std::string>& lines);

struct WavInfo {
    uint32_t rate = 0;
    uint16_t channels = 0;
    size_t data_off = 0;
    size_t data_len = 0;
};

// Accepts only 16-bit PCM, 1 or 2 channels, 8-192 kHz. A data chunk longer than the file is cut to what is there.
std::optional<WavInfo> parse_wav(std::string_view bytes);

struct WavPart {
    std::string_view wav;
    uint32_t silence_before_ms = 0;
};

// One WAV made of the parts in order, each preceded by its silence. All parts must share rate and channel count.
std::optional<std::string> join_wavs(const std::vector<WavPart>& parts);

struct Preset {
    std::wstring id;
    std::wstring name;
    std::filesystem::path emb;
    std::filesystem::path codes;
};

// The preset <voices>\<id> when it passes the checks of discover_presets (its name is the id).
std::optional<Preset> find_preset(const std::filesystem::path& voices_dir, const std::wstring& id);

// <voices>\<id>\speaker.emb.txt + ref_codes.txt (both plain files of sane size); names come from manifest.json when
// it lists the id. Sorted by name.
std::vector<Preset> discover_presets(const std::filesystem::path& voices_dir);

}
