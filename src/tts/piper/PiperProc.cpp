#include "tts/piper/PiperProc.h"

#include "tts/piper/PiperTts.h"

#include <chrono>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <vector>

namespace eb::piper {

namespace {

constexpr size_t kJsonMax = size_t{64} * 1024;
constexpr size_t kMaxLine = 4096;
constexpr size_t kMaxQueuedLines = 64;
constexpr auto kSynthTimeout = std::chrono::seconds(30);
constexpr auto kPoll = std::chrono::milliseconds(25);

bool is_reparse(const fs::path& p) noexcept
{
    const DWORD a = GetFileAttributesW(p.c_str());
    return a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

std::string read_head(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::string s(kJsonMax, '\0');
    f.read(s.data(), static_cast<std::streamsize>(s.size()));
    s.resize(static_cast<size_t>(f.gcount()));
    return s;
}

struct Pipes {
    UniqueHandle in_r, in_w, out_r, out_w, nul;
};

bool make_pipes(Pipes& p) noexcept
{
    HANDLE a = nullptr;
    HANDLE b = nullptr;
    if (!CreatePipe(&a, &b, nullptr, 0))
        return false;
    p.in_r.reset(a);
    p.in_w.reset(b);
    if (!CreatePipe(&a, &b, nullptr, 0))
        return false;
    p.out_r.reset(a);
    p.out_w.reset(b);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    p.nul.reset(CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr));
    return p.nul.valid() && SetHandleInformation(p.in_r.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)
        && SetHandleInformation(p.out_w.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
}

// Restricts inheritance to exactly the listed handles.
class HandleList {
public:
    HandleList(HANDLE* handles, DWORD count) noexcept
    {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        try {
            buf_.resize(size);
        } catch (...) {
            return;
        }
        ok_ = InitializeProcThreadAttributeList(list(), 1, 0, &size) != 0;
        if (ok_
            && !UpdateProcThreadAttribute(list(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, static_cast<PVOID>(handles),
                count * sizeof(HANDLE), nullptr, nullptr)) {
            DeleteProcThreadAttributeList(list());
            ok_ = false;
        }
    }
    ~HandleList()
    {
        if (ok_)
            DeleteProcThreadAttributeList(list());
    }
    HandleList(const HandleList&) = delete;
    HandleList& operator=(const HandleList&) = delete;
    [[nodiscard]] bool ok() const noexcept { return ok_; }
    LPPROC_THREAD_ATTRIBUTE_LIST list() noexcept
    {
        return reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buf_.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    }

private:
    std::vector<char> buf_;
    bool ok_ = false;
};

}

bool safe_exe(const Cfg& c) noexcept
{
    try {
        std::error_code ec;
        const fs::path exe = c.exe();
        if (!c.piper.is_absolute() || is_reparse(c.piper) || is_reparse(exe) || !fs::is_regular_file(exe, ec))
            return false;
        return fs::equivalent(fs::canonical(exe, ec).parent_path(), c.piper, ec) && !ec;
    } catch (...) {
        return false;
    }
}

fs::path voice_model(const Cfg& c, const std::wstring& id)
{
    if (id.empty() || id.size() > 128 || id.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos
        || id.find(L"..") != std::wstring::npos)
        return {};
    std::error_code ec;
    fs::path model = c.voices / (id + L".onnx");
    fs::path json = model;
    json += L".json";
    if (!fs::is_regular_file(model, ec) || !fs::is_regular_file(json, ec))
        return {};
    return model;
}

std::wstring command_line(const Cfg& c, const fs::path& model, int rate_key)
{
    char scale[32];
    std::snprintf(scale, sizeof scale, "%.3f", 20.0 / rate_key);
    std::wstring cmd = quote_arg(c.exe().wstring()) + L" --model " + quote_arg(model.wstring())
        + L" --json-input --output_dir " + quote_arg(c.temp.wstring()) + L" --length_scale ";
    for (const char* s = scale; *s != 0; ++s)
        cmd.push_back(static_cast<wchar_t>(*s));
    return cmd;
}

Proc::~Proc()
{
    if (process_.valid())
        TerminateProcess(process_.get(), 1);
    if (reader_.joinable())
        reader_.join();
    if (process_.valid())
        WaitForSingleObject(process_.get(), 1000);
    process_.reset();
    std::error_code ec;
    for (const auto& p : abandoned)
        fs::remove(p, ec);
}

void Proc::push_line(std::string&& l)
{
    {
        const std::scoped_lock lk(m_);
        if (lines_.size() < kMaxQueuedLines)
            lines_.push_back(std::move(l));
    }
    cv_.notify_all();
}

void Proc::read_loop() noexcept
{
    try {
        std::string cur;
        char buf[1024];
        DWORD n = 0;
        while (ReadFile(out_r_.get(), buf, sizeof buf, &n, nullptr) && n > 0) {
            for (DWORD i = 0; i < n; ++i) {
                if (buf[i] == '\n') {
                    push_line(std::move(cur));
                    cur.clear();
                } else if (buf[i] != '\r' && cur.size() < kMaxLine) {
                    cur.push_back(buf[i]);
                }
            }
        }
    } catch (...) { // NOLINT(bugprone-empty-catch): reader ends, the request then fails as Dead
    }
    {
        const std::scoped_lock lk(m_);
        eof_ = true;
    }
    cv_.notify_all();
}

bool Proc::start(HANDLE job, const Cfg& c, const std::wstring& cmd) noexcept
{
    try {
        Pipes pp;
        if (!make_pipes(pp))
            return false;
        HANDLE inherit[3] = {pp.in_r.get(), pp.out_w.get(), pp.nul.get()};
        HandleList attrs(inherit, 3);
        if (!attrs.ok())
            return false;
        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof si;
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = pp.in_r.get();
        si.StartupInfo.hStdOutput = pp.out_w.get();
        si.StartupInfo.hStdError = pp.nul.get();
        si.lpAttributeList = attrs.list();
        PROCESS_INFORMATION pi{};
        std::wstring line = cmd;
        constexpr DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS
            | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;
        const fs::path exe = c.exe();
        if (!CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, TRUE, flags, nullptr, c.piper.c_str(),
                &si.StartupInfo, &pi))
            return false;
        const UniqueHandle thread(pi.hThread);
        process_.reset(pi.hProcess);
        if (!AssignProcessToJobObject(job, pi.hProcess))
            return false;
        in_w_ = std::move(pp.in_w);
        out_r_ = std::move(pp.out_r);
        reader_ = std::thread([this] { read_loop(); });
        return ResumeThread(pi.hThread) != static_cast<DWORD>(-1);
    } catch (...) {
        return false;
    }
}

bool Proc::alive() noexcept
{
    if (!process_.valid() || WaitForSingleObject(process_.get(), 0) != WAIT_TIMEOUT)
        return false;
    const std::scoped_lock lk(m_);
    return !eof_;
}

bool Proc::send(const std::string& line) noexcept
{
    try {
        const std::string data = line + "\n";
        DWORD written = 0;
        return WriteFile(in_w_.get(), data.data(), static_cast<DWORD>(data.size()), &written, nullptr)
            && written == data.size();
    } catch (...) {
        return false;
    }
}

Wait Proc::await(const std::function<bool()>& cancel) noexcept
{
    try {
        const auto deadline = std::chrono::steady_clock::now() + kSynthTimeout;
        std::unique_lock lk(m_);
        for (;;) {
            while (!lines_.empty()) {
                const std::string l = std::move(lines_.front());
                lines_.pop_front();
                if (!l.ends_with(".wav"))
                    continue;
                if (abandoned.empty())
                    return Wait::Done;
                std::error_code ec;
                fs::remove(abandoned.front(), ec);
                abandoned.pop_front();
            }
            if (eof_)
                return Wait::Dead;
            if (cancel())
                return Wait::Cancelled;
            if (std::chrono::steady_clock::now() > deadline)
                return Wait::Timeout;
            cv_.wait_for(lk, kPoll);
        }
    } catch (...) {
        return Wait::Dead;
    }
}

void clean_stale(const fs::path& dir) noexcept
{
    try {
        std::error_code ec;
        fs::create_directories(dir, ec);
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            const std::wstring name = it->path().filename().wstring();
            if (name.starts_with(L"eb-") && name.ends_with(L".wav"))
                fs::remove(it->path(), ec);
        }
    } catch (...) { // NOLINT(bugprone-empty-catch): stale files are only a disk-space nuisance
    }
}

std::wstring voice_lang(const fs::path& model)
{
    fs::path json = model;
    json += L".json";
    std::wstring lang = piper::lang_from_model_json(read_head(json));
    if (lang.empty()) {
        const std::wstring stem = model.stem().wstring();
        lang = stem.substr(0, stem.find(L'-'));
        std::ranges::replace(lang, L'_', L'-');
    }
    return lang;
}
}
