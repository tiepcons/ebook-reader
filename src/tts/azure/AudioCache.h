#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eb {

inline constexpr uint64_t kDefaultAudioCacheBytes = uint64_t{300} * 1024 * 1024;

// <dir>/<sha256 hex>.mp3 files, least recently used (last-write time) evicted first. Thread-safe.
class AudioCache {
public:
    AudioCache(std::filesystem::path dir, uint64_t cap_bytes) noexcept;

    // sha256 of "voice|rate_pct|text" as 64 lowercase hex characters; empty when hashing fails.
    static std::string key_for(std::string_view voice, int rate_pct, std::string_view text) noexcept;

    // A hit refreshes the entry's recency.
    std::optional<std::vector<std::byte>> get(std::string_view key) noexcept;
    bool put(std::string_view key, std::span<const std::byte> data) noexcept;
    [[nodiscard]] uint64_t total_bytes() noexcept;

private:
    [[nodiscard]] std::filesystem::path path_for(std::string_view key) const;
    void enforce_locked() noexcept;

    std::filesystem::path dir_;
    uint64_t cap_;
    uint64_t total_ = 0;
    std::mutex m_;
};

}
