#pragma once
#include "tts/TtsEngine.h"

#include <memory>

namespace eb {

class WinRtTts final : public TtsEngine {
public:
    WinRtTts();
    ~WinRtTts() override;
    WinRtTts(const WinRtTts&) = delete;
    WinRtTts& operator=(const WinRtTts&) = delete;

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
