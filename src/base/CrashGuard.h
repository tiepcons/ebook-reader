#pragma once
#include "Log.h"
#include "Result.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace eb {

void install_crash_handler(const std::filesystem::path& crash_dir) noexcept;

class CrashLoopGuard {
public:
    explicit CrashLoopGuard(std::filesystem::path marker_file) noexcept;

    [[nodiscard]] std::optional<std::string> previous_suspect() const { return suspect_; }
    Result<void> begin(std::string_view op, std::string_view id);
    void end() noexcept;

    class Scope {
    public:
        Scope(CrashLoopGuard& g, std::string_view op, std::string_view id) : g_(g) 
        {
            if (!g_.begin(op, id))
                log::error("crash marker write failed");
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        ~Scope() { g_.end(); }

    private:
        CrashLoopGuard& g_;
    };

private:
    std::filesystem::path marker_;
    std::optional<std::string> suspect_;
};

}
