#pragma once
#include "tts/azure/AzureSink.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace eb {

using AzureAudio = std::shared_ptr<const std::vector<std::byte>>;

// Plays one MP3 at a time through Windows.Media.Playback on its own MTA thread. Posts Started when playback begins,
// Finished at MediaEnded and Error(kTtsPlayFailed) when the media cannot be decoded; all gated by the sink generation.
class AzurePlayer {
public:
    explicit AzurePlayer(std::shared_ptr<AzureSink> sink);
    ~AzurePlayer();
    AzurePlayer(const AzurePlayer&) = delete;
    AzurePlayer& operator=(const AzurePlayer&) = delete;

    void play(uint32_t gen, uint32_t id, AzureAudio audio, double volume) noexcept;
    void pause() noexcept;
    void resume() noexcept;
    void stop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
