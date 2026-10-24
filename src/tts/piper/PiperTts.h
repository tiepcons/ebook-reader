#pragma once
#include "tts/TtsEngine.h"

#include <filesystem>
#include <memory>

namespace eb::piper {

std::string json_escape(std::string_view utf8);
std::string request_line(std::string_view utf8, std::string_view wav_path_utf8);
std::wstring quote_arg(std::wstring_view arg);
std::wstring lang_from_model_json(std::string_view json);

}

namespace eb {

// Runs the official piper.exe as an isolated child process (one per active voice, kept alive between sentences).
// Voices come from <voices_dir>\<name>.onnx + <name>.onnx.json; voices() is empty when piper.exe is missing.
class PiperTts final : public TtsEngine {
public:
    PiperTts(const std::filesystem::path& piper_dir, const std::filesystem::path& voices_dir,
        const std::filesystem::path& temp_dir);
    ~PiperTts() override;
    PiperTts(const PiperTts&) = delete;
    PiperTts& operator=(const PiperTts&) = delete;

    std::vector<Voice> voices() noexcept override;
    bool speak(uint32_t utterance_id, std::string_view utf8, const std::wstring& voice_id, double rate,
        double volume) noexcept override;
    void prepare(uint32_t utterance_id, std::string_view utf8, const std::wstring& voice_id, double rate) noexcept override;
    void pause() noexcept override;
    void resume() noexcept override;
    void stop() noexcept override;
    void set_sink(HWND hwnd, UINT msg) noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
