#pragma once
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace eb::log {

void init(const std::filesystem::path& file) noexcept;
void shutdown() noexcept;
void write(std::string_view level, std::string_view message) noexcept;

template <class... Args>
void error(std::format_string<Args...> fmt, Args&&... args) noexcept
{
    try {
        write("ERROR", std::format(fmt, std::forward<Args>(args)...));
    } catch (...) { // NOLINT(bugprone-empty-catch): logging must never throw
    }
}

template <class... Args>
void info(std::format_string<Args...> fmt, Args&&... args) noexcept
{
    try {
        write("INFO", std::format(fmt, std::forward<Args>(args)...));
    } catch (...) { // NOLINT(bugprone-empty-catch): logging must never throw
    }
}

// Compiled out of release builds.
template <class... Args>
void debug([[maybe_unused]] std::format_string<Args...> fmt, [[maybe_unused]] Args&&... args) noexcept
{
#ifndef NDEBUG
    try {
        write("DEBUG", std::format(fmt, std::forward<Args>(args)...));
    } catch (...) { // NOLINT(bugprone-empty-catch): logging must never throw
    }
#endif
}

}
