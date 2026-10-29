#include "base/FileIo.h"
#include "base/Text.h"
#include "tts/vieneu/VieNeuEngine.h"

#include <algorithm>
#include <charconv>
#include <set>

namespace eb::vieneu::engine {

namespace fs = std::filesystem;
using piper::Cancel;
using piper::Wait;

namespace {

constexpr size_t kBatchMax = 6;
constexpr size_t kLowWater = 2;
constexpr auto kCoalesce = std::chrono::milliseconds(40);
constexpr auto kBaseLimit = std::chrono::seconds(30);
constexpr auto kMaxLimit = std::chrono::seconds(300);
constexpr std::string_view kOutPrefix = "audio_out=";

std::vector<std::string> lines_of(std::string_view text)
{
    if (const auto h = split_heading(text)) {
        std::string head = batch_line(h->head);
        std::string rest = batch_line(h->rest);
        if (!head.empty() && !rest.empty())
            return {std::move(head), std::move(rest)};
    }
    std::string one = batch_line(text);
    if (one.empty())
        return {};
    return {std::move(one)};
}

std::optional<std::string> read_all(const fs::path& p)
{
    std::error_code ec;
    if (piper::is_reparse(p) || !fs::is_regular_file(p, ec))
        return std::nullopt;
    const auto size = fs::file_size(p, ec);
    if (ec || size > kMaxWavBytes)
        return std::nullopt;
    auto s = eb::read_all(p, kMaxWavBytes);
    if (!s)
        return std::nullopt;
    return std::move(*s);
}

bool write_all(const fs::path& p, std::string_view data)
{
    return eb::write_file(p, data);
}

// Posts the error for the utterance the caller waits on and forgets the failed items.
void fail_items(Engine& e, const std::vector<uint32_t>& ids, uint32_t code)
{
    for (const uint32_t id : ids) {
        const auto it = e.items.find(id);
        if (it == e.items.end())
            continue;
        if (code != kTtsBadVoice)
            e.sh->note(it->second.key.voice, false);
        if (e.want && e.want->id == id) {
            e.sh->post(e.want->gen, id, TtsEvent::Error, code);
            e.want.reset();
        }
        discard_item(e, id);
    }
}

std::vector<uint32_t> todo_run(const Engine& e)
{
    std::vector<uint32_t> ids;
    const std::wstring* voice = nullptr;
    for (const auto& [id, it] : e.items) {
        if (it.ready || it.queued)
            continue;
        if (voice == nullptr)
            voice = &it.key.voice;
        if (it.key.voice != *voice || ids.size() >= kBatchMax)
            break;
        ids.push_back(id);
    }
    return ids;
}

std::chrono::seconds batch_limit(const std::vector<std::string>& lines)
{
    size_t bytes = 0;
    for (const std::string& l : lines)
        bytes += l.size();
    return std::min(kBaseLimit + std::chrono::seconds(bytes / 100), kMaxLimit);
}

bool start_proc(Engine& e, Batch& b, const Verified& v, const Preset& voice, const std::vector<std::string>& lines)
{
    std::error_code ec;
    const fs::path txt = b.dir / L"in.txt";
    if (!fs::create_directory(b.dir, ec) || !write_all(txt, batch_file(lines)))
        return false;
    b.proc = std::make_unique<piper::Proc>();
    return b.proc->start(e.job.get(), e.cfg, command_line(e.cfg, v.assets, voice, txt, b.dir, child_threads()));
}

void start_batch(Engine& e, const std::vector<uint32_t>& ids)
{
    const std::wstring voice_id = e.items.at(ids[0]).key.voice;
    const auto v = e.verdict->get();
    if (!v) {
        fail_items(e, ids, kTtsSynthFailed);
        return;
    }
    const auto voice = std::ranges::find(v->presets, voice_id, &Preset::id);
    if (voice == v->presets.end()) {
        fail_items(e, ids, kTtsBadVoice);
        return;
    }
    Batch b;
    std::vector<std::string> lines;
    for (const uint32_t id : ids) {
        Item& it = e.items.at(id);
        for (size_t part = 0; part < it.lines.size(); ++part) {
            b.pieces.push_back({id, part, false});
            lines.push_back(it.lines[part]);
        }
        it.queued = true;
    }
    b.dir = e.cfg.temp / (L"eb-b" + std::to_wstring(++e.serial));
    b.deadline = Clock::now() + batch_limit(lines);
    if (!start_proc(e, b, *v, *voice, lines)) {
        b.proc.reset();
        proc::remove_tree(b.dir);
        fail_items(e, ids, kTtsSynthFailed);
        return;
    }
    e.batch = std::move(b);
}

void fail_item(Engine& e, uint32_t id)
{
    fail_items(e, {id}, kTtsSynthFailed);
}

std::optional<size_t> piece_of(const Batch& b, const fs::path& path)
{
    std::error_code ec;
    const std::wstring name = path.filename().wstring();
    if (!name.starts_with(L"line_") || !name.ends_with(L".wav") || !fs::equivalent(path.parent_path(), b.dir, ec))
        return std::nullopt;
    const std::string digits = wide_to_utf8(name.substr(5, name.size() - 9)).value_or(std::string{});
    size_t k = 0;
    const auto r = std::from_chars(digits.data(), digits.data() + digits.size(), k);
    if (digits.empty() || r.ec != std::errc{} || r.ptr != digits.data() + digits.size() || k < 1 || k > b.pieces.size())
        return std::nullopt;
    return k - 1;
}

void on_line(Engine& e, const std::string& line)
{
    if (!e.batch || !line.starts_with(kOutPrefix))
        return;
    Batch& b = *e.batch;
    const auto wide = utf8_to_wide(std::string_view(line).substr(kOutPrefix.size()));
    const auto k = wide ? piece_of(b, fs::path(*wide)) : std::nullopt;
    if (!k || b.pieces[*k].done)
        return;
    Piece& piece = b.pieces[*k];
    piece.done = true;
    ++b.done;
    const auto it = e.items.find(piece.item);
    if (it == e.items.end())
        return;
    it->second.raw[piece.part] = b.dir / (L"line_" + std::to_wstring(*k + 1) + L".wav");
    if (++it->second.got == it->second.lines.size())
        finalize_item(e, piece.item);
}

// Ends the running batch (killing its child if still alive); utterances it did not deliver fail.
void end_batch(Engine& e)
{
    if (!e.batch)
        return;
    std::set<uint32_t> unfinished;
    for (const Piece& p : e.batch->pieces) {
        const auto it = e.items.find(p.item);
        if (it != e.items.end() && !it->second.ready)
            unfinished.insert(p.item);
    }
    const fs::path dir = e.batch->dir;
    e.batch.reset();
    proc::remove_tree(dir);
    fail_items(e, std::vector<uint32_t>(unfinished.begin(), unfinished.end()), kTtsSynthFailed);
    e.trash.retry();
    schedule(e);
}

}

void finalize_item(Engine& e, uint32_t id)
{
    Item& it = e.items.at(id);
    std::vector<std::string> data;
    data.reserve(it.raw.size());
    for (const fs::path& p : it.raw) {
        auto bytes = read_all(p);
        if (!bytes) {
            it.queued = false;
            fail_item(e, id);
            return;
        }
        data.push_back(std::move(*bytes));
    }
    std::vector<WavPart> parts;
    parts.reserve(data.size());
    for (size_t i = 0; i < data.size(); ++i)
        parts.push_back({data[i], i == 0 ? (it.lead ? kLeadSilenceMs : 0) : kHeadingPauseMs});
    const auto joined = join_wavs(parts);
    const fs::path wav = play_dir(e.cfg) / (L"eb-" + std::to_wstring(++e.serial) + L".wav");
    std::error_code rm;
    fs::remove(wav, rm);
    if (!joined || !eb::write_new(wav, *joined)) {
        e.trash.discard(wav);
        it.queued = false;
        fail_item(e, id);
        return;
    }
    for (const fs::path& p : it.raw)
        e.trash.discard(p);
    it.raw.assign(it.raw.size(), fs::path{});
    it.wav = wav;
    it.ready = true;
    it.queued = false;
    e.sh->note(it.key.voice, true);
    try_play(e);
}

bool add_item(Engine& e, const ReqPtr& req, bool lead)
{
    const Key key{req->id, std::hash<std::string_view>{}(req->text), req->voice};
    if (const auto it = e.items.find(req->id); it != e.items.end()) {
        if (it->second.key == key)
            return true;
        discard_item(e, req->id);
    }
    std::vector<std::string> lines = lines_of(req->text);
    if (lines.empty())
        return false;
    Item& it = e.items[req->id];
    it.key = key;
    it.lead = lead;
    it.raw.resize(lines.size());
    it.lines = std::move(lines);
    return true;
}

void discard_item(Engine& e, uint32_t id)
{
    const auto it = e.items.find(id);
    if (it == e.items.end())
        return;
    if (it->second.queued && e.batch)
        abort_batch(e);
    for (const fs::path& p : it->second.raw)
        e.trash.discard(p);
    e.trash.discard(it->second.wav);
    e.items.erase(id);
}

void abort_batch(Engine& e)
{
    if (!e.batch)
        return;
    const fs::path dir = e.batch->dir;
    e.batch.reset();
    proc::remove_tree(dir);
    for (auto& [id, it] : e.items) {
        if (!it.ready) {
            it.got = 0;
            it.raw.assign(it.raw.size(), fs::path{});
        }
        it.queued = false;
    }
    schedule(e);
}

void schedule(Engine& e)
{
    if (e.batch || e.start_at)
        return;
    size_t todo = 0;
    size_t ready = 0;
    for (const auto& [id, it] : e.items) {
        if (it.ready)
            ++ready;
        else if (!it.queued)
            ++todo;
    }
    if (todo > 0 && (e.want || todo >= kBatchMax || ready <= kLowWater))
        e.start_at = Clock::now() + kCoalesce;
}

void maybe_start(Engine& e)
{
    e.start_at.reset();
    if (e.batch)
        return;
    const std::vector<uint32_t> ids = todo_run(e);
    if (!ids.empty())
        start_batch(e, ids);
    e.trash.retry();
}

void pump(Engine& e)
{
    if (!e.batch)
        return;
    const Batch& b = *e.batch;
    const Clock::time_point deadline = b.deadline;
    std::string line;
    const Cancel cancel = [&e] { return e.sh->quit || e.queue->has_work(); };
    const Wait w = b.proc->await(cancel, std::chrono::seconds(1), &line);
    if (w == Wait::Done)
        on_line(e, line);
    const bool over = w == Wait::Dead || (w != Wait::Done && Clock::now() > deadline);
    if (e.batch && (over || e.batch->done == e.batch->pieces.size()))
        end_batch(e);
}

}
