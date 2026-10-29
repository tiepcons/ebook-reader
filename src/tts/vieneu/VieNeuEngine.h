#pragma once
#include "tts/piper/PiperProc.h"
#include "tts/proc/ProcShared.h"
#include "tts/vieneu/VieNeuProc.h"
#include "tts/vieneu/VieNeuVerify.h"

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// Worker-thread state of VieNeuTts. Everything here runs on the single worker thread except Shared, which the UI
// thread also touches.
namespace eb::vieneu::engine {

using Clock = std::chrono::steady_clock;
using proc::Shared;

struct Req {
    uint32_t gen = 0;
    uint32_t epoch = 0;
    uint32_t id = 0;
    std::string text;
    std::wstring voice;
    double rate = 1.0;
    double volume = 0.0;
};
using ReqPtr = std::shared_ptr<const Req>;

struct Key {
    uint32_t id = 0;
    size_t hash = 0;
    std::wstring voice;
    bool operator==(const Key&) const = default;
};

// One utterance the engine knows about. Its text becomes one batch line, or two when a heading is split off.
struct Item {
    Key key;
    bool lead = false;   // 300 ms of silence in front (first utterance after a start)
    bool queued = false; // its lines are in the running batch
    bool ready = false;
    std::vector<std::string> lines;
    std::vector<std::filesystem::path> raw; // one finished line file per line
    size_t got = 0;
    std::filesystem::path wav; // stitched audio, once ready
};

struct Piece {
    uint32_t item = 0;
    size_t part = 0;
    bool done = false;
};

// One audiocpp_cli run; line k of the batch file (1-based) comes back as <dir>\line_<k>.wav.
struct Batch {
    std::unique_ptr<piper::Proc> proc;
    std::filesystem::path dir;
    std::vector<Piece> pieces;
    size_t done = 0;
    Clock::time_point deadline;
};

struct Playing {
    WinRtPlayer player;
    std::filesystem::path wav;
};

// Result of the background hash check; `v` stays null (and the engine hidden) unless the install matched its pins.
struct Verdict {
    std::mutex m;
    std::shared_ptr<const Verified> v;
    std::atomic<bool> done{false};

    [[nodiscard]] std::shared_ptr<const Verified> get() noexcept
    {
        const std::scoped_lock lk(m);
        return v;
    }
};

struct Engine;
using Queue = proc::Queue<Engine>;

struct Engine {
    piper::Cfg cfg;
    std::shared_ptr<Shared> sh;
    std::shared_ptr<Queue> queue;
    std::shared_ptr<Verdict> verdict;
    UniqueHandle job;
    proc::Trash trash;
    std::map<uint32_t, Item> items;
    std::optional<Batch> batch;
    ReqPtr want; // the utterance speak() asked for, until its audio starts
    Playing cur;
    std::optional<Clock::time_point> start_at;
    bool fresh = true; // nothing played since the engine started or was stopped
    uint32_t serial = 0;
};

void do_speak(Engine& e, const ReqPtr& req);
void do_prepare(Engine& e, const ReqPtr& req);
void do_stop(Engine& e);
void close_cur(Engine& e);
void try_play(Engine& e);

bool add_item(Engine& e, const ReqPtr& req, bool lead);
// Stitches the finished line files of item `id` into its WAV and starts it when the caller waits for it. A broken item
// fails alone; the rest of the batch goes on.
void finalize_item(Engine& e, uint32_t id);
void discard_item(Engine& e, uint32_t id);
void abort_batch(Engine& e);
void schedule(Engine& e);
void maybe_start(Engine& e);
void pump(Engine& e);

}
