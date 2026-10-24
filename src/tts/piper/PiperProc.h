#pragma once
#include "base/Handle.h"

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace eb::piper {

namespace fs = std::filesystem;

struct Cfg {
    fs::path piper;
    fs::path voices;
    fs::path temp;
    fs::path exe() const { return piper / L"piper.exe"; }
};

// True when piper.exe is an absolute, regular, non-reparse file that really lives inside Cfg::piper.
bool safe_exe(const Cfg& c) noexcept;
// Empty unless <voices>\<id>.onnx and its .onnx.json exist and id is a plain file stem.
fs::path voice_model(const Cfg& c, const std::wstring& id);
// rate_key is the playback rate in 0.05 steps; piper gets length_scale = 1 / rate.
std::wstring command_line(const Cfg& c, const fs::path& model, int rate_key);

// Language tag for a voice model: json "language.code" with a fallback to the file stem ("vi_VN-x" -> "vi-VN").
std::wstring voice_lang(const fs::path& model);
// Creates the temp dir and removes stale eb-*.wav files left by a previous run.
void clean_stale(const fs::path& dir) noexcept;

enum class Wait : uint8_t { Done, Dead, Timeout, Cancelled };

// One piper.exe child plus the thread that turns its stdout into lines. Used from a single worker thread.
struct Proc {
    std::wstring voice;
    int rate = 0;
    std::deque<fs::path> abandoned;

    Proc() = default;
    Proc(const Proc&) = delete;
    Proc& operator=(const Proc&) = delete;
    ~Proc();

    bool start(HANDLE job, const Cfg& c, const std::wstring& cmd) noexcept;
    bool alive() noexcept;
    bool send(const std::string& line) noexcept;
    // Waits for the next ".wav" line; wavs listed in `abandoned` are skipped and deleted first.
    Wait await(const std::function<bool()>& cancel) noexcept;

private:
    void push_line(std::string&& l);
    void read_loop() noexcept;

    UniqueHandle process_;
    UniqueHandle in_w_;
    UniqueHandle out_r_;
    std::thread reader_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::string> lines_;
    bool eof_ = false;
};

}
