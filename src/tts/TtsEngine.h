#pragma once
#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace eb {

struct Voice {
    std::wstring id;
    std::wstring name;
    std::wstring lang;
};

enum class TtsEvent : uint8_t { Started, Word, Finished, Error };

enum TtsError : uint8_t { kTtsBadVoice = 1, kTtsSynthFailed = 2, kTtsPlayFailed = 3, kTtsTextTooLarge = 4,
    kTtsAuth = 5, kTtsNetwork = 6, kTtsQuota = 7 };

inline constexpr size_t kTtsMaxUtf8 = size_t{64} * 1024;
inline constexpr uint32_t kTtsValueMask = 0xFFFFFF;

constexpr LPARAM pack_tts(TtsEvent ev, uint32_t value) noexcept
{
    return static_cast<LPARAM>((static_cast<uint32_t>(ev) << 24) | (value & kTtsValueMask));
}

constexpr TtsEvent tts_event(LPARAM lp) noexcept
{
    return static_cast<TtsEvent>((static_cast<uint32_t>(lp) >> 24) & 0xFF);
}

constexpr uint32_t tts_value(LPARAM lp) noexcept
{
    return static_cast<uint32_t>(lp) & kTtsValueMask;
}

// Events are posted to the sink window: wParam = utterance id, lParam = pack_tts(event, utf8 offset | error code).
// After stop() returns, no event of an earlier utterance is posted.
class TtsEngine {
public:
    virtual ~TtsEngine() = default;
    virtual std::vector<Voice> voices() noexcept = 0;
    virtual bool speak(uint32_t utterance_id, std::string_view utf8, const std::wstring& voice_id, double rate,
        double volume) noexcept = 0;
    virtual void prepare(uint32_t utterance_id, std::string_view utf8, const std::wstring& voice_id, double rate) noexcept = 0;
    virtual void pause() noexcept = 0;
    virtual void resume() noexcept = 0;
    virtual void stop() noexcept = 0;
    virtual void set_sink(HWND hwnd, UINT msg) noexcept = 0;
};

}
