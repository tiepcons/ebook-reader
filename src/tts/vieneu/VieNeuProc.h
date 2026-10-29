#pragma once
#include "tts/piper/PiperProc.h"
#include "tts/vieneu/VieNeuText.h"

#include <optional>

// Files and command line of the audiocpp_cli child that VieNeuTts starts once per batch of sentences. The child is
// started through piper::Proc, so it runs at Low integrity inside the engine's job object.
namespace eb::vieneu {

namespace fs = std::filesystem;

// piper::Cfg meaning: `piper` = libs\vieneu (audiocpp_cli.exe and its DLLs), `voices` = data\vieneu, `temp` = work dir.
piper::Cfg make_cfg(const fs::path& libs_dir, const fs::path& data_dir, const fs::path& temp_dir);

struct Assets {
    fs::path model;    // data\vieneu\vieneu*.gguf
    fs::path g2p_dict; // data\vieneu\sea_g2p.bin
    fs::path g2p_lib;  // libs\vieneu\sea_g2p_rs.dll
};

// All of: a safe audiocpp_cli.exe, sea_g2p_rs.dll, one VieNeu GGUF (GGUF magic) and sea_g2p.bin, each a plain file of
// a believable size. Nothing is read beyond four header bytes.
std::optional<Assets> find_assets(const piper::Cfg& c);

inline fs::path voices_dir(const piper::Cfg& c)
{
    return c.voices / L"voices";
}

// Folder for the stitched WAVs the player reads: next to `temp`, writable by the engine only (the child runs at Low
// integrity and cannot create or replace files there).
inline fs::path play_dir(const piper::Cfg& c)
{
    return c.temp.parent_path() / (c.temp.filename().wstring() + L"-play");
}
// Creates play_dir (refusing reparse points on its path) and sweeps leftovers of an earlier run.
bool prepare_play_dir(const fs::path& dir) noexcept;

// Worker threads for the child: half the cores, 1 to 4.
unsigned child_threads() noexcept;

std::wstring command_line(const piper::Cfg& c, const Assets& a, const Preset& voice, const fs::path& batch_txt,
    const fs::path& out_dir, unsigned threads);

}
