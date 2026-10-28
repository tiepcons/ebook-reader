#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

namespace eb::piper {

enum class PlayEvent : uint8_t { Started, Finished, Failed };

// Initialises the WinRT multithreaded apartment for the calling thread.
class Apartment {
public:
    Apartment() noexcept;
    ~Apartment();
    Apartment(const Apartment&) = delete;
    Apartment& operator=(const Apartment&) = delete;
    [[nodiscard]] bool ok() const noexcept { return ok_; }

private:
    bool ok_ = false;
};

// Cheap shared handle around a MediaPlayer playing one WAV file. Copies share the player, so pause/resume/close
// may be called from any thread; events are delivered on WinRT threads.
class Player {
public:
    bool open(const std::filesystem::path& wav) noexcept;
    [[nodiscard]] bool valid() const noexcept { return impl_ != nullptr; }
    bool play(double volume, std::function<void(PlayEvent)> on_event) noexcept;
    void pause() noexcept;
    void resume() noexcept;
    void close() noexcept;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}
