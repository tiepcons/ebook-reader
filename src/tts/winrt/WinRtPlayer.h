#pragma once
#include "tts/TtsEngine.h"

#include <unknwn.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>

namespace eb {

enum class PlayEvent : uint8_t { Started, Finished, Failed, Word };

// Initialises the WinRT multithreaded apartment for the calling thread.
class WinRtApartment {
public:
    WinRtApartment() noexcept;
    ~WinRtApartment();
    WinRtApartment(const WinRtApartment&) = delete;
    WinRtApartment& operator=(const WinRtApartment&) = delete;
    [[nodiscard]] bool ok() const noexcept { return ok_; }

private:
    bool ok_ = false;
};

// Cheap shared handle around one Windows.Media.Playback.MediaPlayer. Copies share the player, so pause/resume/close
// may be called from any thread; events are delivered on WinRT threads. Word carries the SpeechWord cue's
// StartPositionInInput and is only raised for synthesizer streams that include word-boundary metadata.
class WinRtPlayer {
public:
    using OnEvent = std::function<void(PlayEvent, uint32_t)>;

    bool open_file(const std::filesystem::path& wav) noexcept;
    bool open_bytes(std::span<const std::byte> audio, const wchar_t* mime) noexcept;
    // stream is any WinRT object implementing IRandomAccessStream, passed as its IUnknown.
    bool open_stream(IUnknown* stream, const wchar_t* mime) noexcept;
    [[nodiscard]] bool valid() const noexcept { return impl_ != nullptr; }
    // rate is the playback speed (1.0 = as recorded).
    bool play(double volume, OnEvent on_event, double rate = 1.0) noexcept;
    void pause() noexcept;
    void resume() noexcept;
    void close() noexcept;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

struct KeepState;

// Loops an inaudible silent clip so the audio endpoint stays awake between utterances; otherwise it swallows the
// first words of each one. A running stream blocks system idle sleep, so pause() it whenever reading is paused and
// stop() it when done. One detached MTA worker serves a whole session and the calls never block the caller. No audio
// device or any WinRT failure just means no keep-alive. audible=false only tracks the state (tests). Single thread.
class AudioKeepAlive {
public:
    explicit AudioKeepAlive(bool audible = true) noexcept;
    ~AudioKeepAlive();
    AudioKeepAlive(const AudioKeepAlive&) = delete;
    AudioKeepAlive& operator=(const AudioKeepAlive&) = delete;
    // Begins the session, or resumes it after pause().
    void start() noexcept;
    void pause() noexcept;
    void stop() noexcept;
    // True while started and not paused, whether or not the stream could be opened.
    [[nodiscard]] bool wanted() const noexcept { return wanted_; }

private:
    static void run(const std::shared_ptr<KeepState>& s) noexcept;
    std::shared_ptr<KeepState> st_;
    bool audible_;
    bool wanted_ = false;
};

}
