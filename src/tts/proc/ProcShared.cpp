#include "tts/proc/ProcShared.h"

namespace eb::proc {

bool Shared::current(uint32_t g) noexcept
{
    const std::scoped_lock lk(m);
    return g == gen;
}

bool Shared::epoch_is(uint32_t e) noexcept
{
    const std::scoped_lock lk(m);
    return e == epoch;
}

uint32_t Shared::epoch_now() noexcept
{
    const std::scoped_lock lk(m);
    return epoch;
}

uint32_t Shared::bump_gen() noexcept
{
    const std::scoped_lock lk(m);
    return ++gen;
}

void Shared::invalidate_all() noexcept
{
    const std::scoped_lock lk(m);
    ++gen;
    ++epoch;
}

void Shared::post(uint32_t g, uint32_t id, TtsEvent ev, uint32_t value) noexcept
{
    const std::scoped_lock lk(m);
    if (g == gen && hwnd != nullptr)
        PostMessageW(hwnd, msg, id, pack_tts(ev, value));
}

bool Shared::voice_dead(const std::wstring& v) noexcept
{
    try {
        const std::scoped_lock lk(m);
        const auto it = fails.find(v);
        return it != fails.end() && it->second >= 2;
    } catch (...) {
        return true;
    }
}

void Shared::note(const std::wstring& v, bool ok) noexcept
{
    try {
        const std::scoped_lock lk(m);
        int& n = fails[v];
        n = ok ? 0 : n + 1;
    } catch (...) { // NOLINT(bugprone-empty-catch): the counter is advisory
    }
}

void Shared::set_playing(WinRtPlayer p) noexcept
{
    const std::scoped_lock lk(pm);
    playing = std::move(p);
}

void Shared::with_playing(bool play) noexcept
{
    WinRtPlayer p;
    {
        const std::scoped_lock lk(pm);
        p = playing;
    }
    if (play)
        p.resume();
    else
        p.pause();
}

UniqueHandle make_job(DWORD max_procs, SIZE_T memory) noexcept
{
    UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_JOB_MEMORY
        | JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    info.BasicLimitInformation.ActiveProcessLimit = max_procs;
    info.JobMemoryLimit = memory;
    JOBOBJECT_BASIC_UI_RESTRICTIONS ui{};
    ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_HANDLES | JOB_OBJECT_UILIMIT_READCLIPBOARD | JOB_OBJECT_UILIMIT_WRITECLIPBOARD
        | JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS | JOB_OBJECT_UILIMIT_GLOBALATOMS
        | JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_EXITWINDOWS;
    if (!job.valid() || !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &info, sizeof info)
        || !SetInformationJobObject(job.get(), JobObjectBasicUIRestrictions, &ui, sizeof ui))
        job.reset();
    return job;
}

namespace {

bool is_real_dir(DWORD attr) noexcept
{
    return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0 && (attr & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

}

void remove_tree(const std::filesystem::path& p) noexcept
{
    constexpr size_t kMaxEntries = 100000;
    try {
        namespace fs = std::filesystem;
        std::vector<fs::path> dirs; // real folders in discovery order; removed last-first once emptied
        std::vector<fs::path> todo{p};
        size_t seen = 0;
        while (!todo.empty() && seen++ < kMaxEntries) {
            const fs::path cur = std::move(todo.back());
            todo.pop_back();
            const DWORD attr = GetFileAttributesW(cur.c_str());
            if (attr == INVALID_FILE_ATTRIBUTES)
                continue;
            if (is_real_dir(attr)) {
                dirs.push_back(cur);
                std::error_code ec;
                for (fs::directory_iterator it(cur, ec), end; !ec && it != end; it.increment(ec))
                    todo.push_back(it->path());
            } else if ((attr & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                RemoveDirectoryW(cur.c_str());
            } else {
                DeleteFileW(cur.c_str());
            }
        }
        for (auto it = dirs.rbegin(); it != dirs.rend(); ++it)
            RemoveDirectoryW(it->c_str());
    } catch (...) { // NOLINT(bugprone-empty-catch): leftovers are swept at the next start
    }
}

void Trash::discard(const std::filesystem::path& p) noexcept
{
    if (p.empty())
        return;
    try {
        std::error_code ec;
        std::filesystem::remove(p, ec);
        if (ec)
            pending.push_back(p);
    } catch (...) { // NOLINT(bugprone-empty-catch): stale files are removed at the next start
    }
}

void Trash::retry() noexcept
{
    try {
        std::vector<std::filesystem::path> again;
        again.swap(pending);
        for (const auto& p : again)
            discard(p);
    } catch (...) { // NOLINT(bugprone-empty-catch): stale files are removed at the next start
    }
}

bool play_wav(const std::shared_ptr<Shared>& sh, WinRtPlayer& player, double volume, double rate, uint32_t gen,
    uint32_t id) noexcept
{
    return player.play(
        volume,
        [sh, gen, id](PlayEvent ev, uint32_t) {
            switch (ev) {
            case PlayEvent::Started: sh->post(gen, id, TtsEvent::Started, 0); break;
            case PlayEvent::Finished: sh->post(gen, id, TtsEvent::Finished, 0); break;
            case PlayEvent::Failed: sh->post(gen, id, TtsEvent::Error, kTtsPlayFailed); break;
            case PlayEvent::Word: break;
            }
        },
        rate);
}

}
