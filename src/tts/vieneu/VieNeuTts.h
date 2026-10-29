#pragma once
#include "tts/TtsEngine.h"

#include <filesystem>
#include <memory>

namespace eb {

// Optional offline engine: audio.cpp's audiocpp_cli running the VieNeu-TTS v3 Turbo GGUF as a Low-integrity child, one
// process per batch of upcoming sentences (the ~0.5 s model load is paid once per batch). No server, no network.
// Needs libs_dir = libs\vieneu (audiocpp_cli.exe, sea_g2p_rs.dll, runtime DLLs) and data_dir = data\vieneu (the
// vieneu*.gguf model, sea_g2p.bin and voices\<preset>\{speaker.emb.txt,ref_codes.txt}); voices() is empty when anything
// is missing or implausible. Each preset is offered for both Vietnamese and English.
class VieNeuTts final : public TtsEngine {
public:
    VieNeuTts(const std::filesystem::path& libs_dir, const std::filesystem::path& data_dir,
        const std::filesystem::path& temp_dir);
    ~VieNeuTts() override;
    VieNeuTts(const VieNeuTts&) = delete;
    VieNeuTts& operator=(const VieNeuTts&) = delete;

    std::vector<Voice> voices() noexcept override;
    bool speak(uint32_t utterance_id, std::string_view utf8, const std::wstring& voice_id, double rate,
        double volume) noexcept override;
    void prepare(uint32_t utterance_id, std::string_view utf8, const std::wstring& voice_id, double rate) noexcept override;
    void pause() noexcept override;
    void resume() noexcept override;
    void stop() noexcept override;
    void set_sink(HWND hwnd, UINT msg) noexcept override;
    [[nodiscard]] size_t lookahead() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
