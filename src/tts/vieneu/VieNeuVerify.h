#pragma once
#include "tts/vieneu/VieNeuProc.h"

#include <string_view>
#include <vector>

// The files the engine feeds to its child are only used when their SHA-256 equals a value pinned in the app (the pins
// come from tools/vieneu_pins.psd1 through the generated VieNeuPins.h). Anything else is ignored.
namespace eb::vieneu {

struct Pin {
    std::string_view name; // "<preset id>/<file>"
    std::string_view sha256;
};

struct Pins {
    std::string_view model;
    std::string_view dict;
    std::string_view manifest; // voices\manifest.json (display names only); empty = names are never taken from it
    std::vector<Pin> voice_files;
};

const Pins& builtin_pins();

// Lowercase hex SHA-256 of a plain file of at most `max_bytes`; empty when it cannot be read.
std::string sha256_file(const fs::path& file, uintmax_t max_bytes) noexcept;

struct Verified {
    Assets assets;
    std::vector<Preset> presets; // only presets whose two files match their pins
};

// find_assets + hash checks of the model, the dictionary and every preset file. Hashes are cached in `cache_file`
// keyed by path, size and modification time, so a repeat run costs only directory scans.
std::optional<Verified> verify_install(const piper::Cfg& c, const Pins& pins, const fs::path& cache_file) noexcept;

inline fs::path cache_file_of(const piper::Cfg& c)
{
    return c.temp.parent_path() / (c.temp.filename().wstring() + L"-verified.txt");
}

}
