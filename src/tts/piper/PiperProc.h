#pragma once
#include "base/Handle.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace eb::piper {

namespace fs = std::filesystem;

// Where a child synthesizer lives: `piper` is its folder (also its PATH and working directory), `voices` the data it
// reads, `temp` the Low-integrity folder it writes to. VieNeu reuses this with its own folders and exe_name.
struct Cfg {
    fs::path piper;
    fs::path voices;
    fs::path temp;
    fs::path exe_name = L"piper.exe";
    fs::path exe() const { return piper / exe_name; }
};

// True for reparse points and for paths that do not exist.
bool is_reparse(const fs::path& p) noexcept;
// True when the exe is an absolute, regular, non-reparse file that really lives inside Cfg::piper.
bool safe_exe(const Cfg& c) noexcept;
// Empty unless <voices>\<id>.onnx and its .onnx.json exist and id is a plain file stem.
fs::path voice_model(const Cfg& c, const std::wstring& id);
// rate_key is the playback rate in 0.05 steps; piper gets length_scale = 1 / rate.
std::wstring command_line(const Cfg& c, const fs::path& model, int rate_key);

// Language tag for a voice model: json "language.code" with a fallback to the file stem ("vi_VN-x" -> "vi-VN").
std::wstring voice_lang(const fs::path& model);
// Creates the temp dir, refuses reparse points on its path, labels it Low integrity (the child runs at Low) and
// removes stale eb-*.wav files left by a previous run.
bool prepare_temp_dir(const fs::path& dir) noexcept;

enum class Wait : uint8_t { Done, Dead, Timeout, Cancelled };

using Cancel = std::function<bool()>;

// One piper.exe child (Low integrity, inside the caller's job) plus the thread that turns its stdout into lines.
// Used from a single worker thread; the destructor kills the child.
struct Proc {
    std::wstring voice;
    int rate = 0;

    Proc() = default;
    Proc(const Proc&) = delete;
    Proc& operator=(const Proc&) = delete;
    ~Proc();

    bool start(HANDLE job, const Cfg& c, const std::wstring& cmd) noexcept;
    bool alive() noexcept;
    // Never blocks past a few seconds: the write is overlapped and abandoned when cancel() turns true.
    bool send(const std::string& line, const Cancel& cancel) noexcept;
    // Waits for the next stdout line that ends in ".wav" (other lines are dropped); `line` receives it when not null.
    Wait await(const Cancel& cancel, std::chrono::seconds limit, std::string* line = nullptr) noexcept;

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
