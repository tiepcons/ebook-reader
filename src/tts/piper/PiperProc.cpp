#include "tts/piper/PiperProc.h"

#include "base/PathSafety.h"
#include "tts/piper/PiperTts.h"

#include <aclapi.h>
#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <vector>

namespace eb::piper {

namespace {

constexpr size_t kJsonMax = size_t{64} * 1024;
constexpr size_t kMaxLine = 4096;
constexpr size_t kMaxQueuedLines = 64;
constexpr DWORD kStdinBuffer = 512 * 1024;
static_assert(kStdinBuffer >= 6 * kTtsMaxUtf8 + 4096, "one request line must always fit the pipe buffer");
constexpr auto kPoll = std::chrono::milliseconds(25);
constexpr auto kWriteLimit = std::chrono::seconds(5);

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

// stdin is a named pipe so the parent end can use overlapped I/O (a wedged child must never block the engine).
bool make_stdin_pipe(Pipes& p) noexcept
{
    static std::atomic<uint32_t> serial{0};
    try {
        const std::wstring name = L"\\\\.\\pipe\\eb-piper-" + std::to_wstring(GetCurrentProcessId()) + L"-"
            + std::to_wstring(serial.fetch_add(1));
        p.in_w.reset(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, kStdinBuffer, 0, 0, nullptr));
        if (!p.in_w.valid())
            return false; // never connect to a pipe another process created under our name
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        p.in_r.reset(CreateFileW(name.c_str(), GENERIC_READ, 0, &sa, OPEN_EXISTING,
            SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS, nullptr));
        return p.in_w.valid() && p.in_r.valid();
    } catch (...) {
        return false;
    }
}

bool make_pipes(Pipes& p) noexcept
{
    HANDLE a = nullptr;
    HANDLE b = nullptr;
    if (!make_stdin_pipe(p) || !CreatePipe(&a, &b, nullptr, 0))
        return false;
    p.out_r.reset(a);
    p.out_w.reset(b);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    p.nul.reset(CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr));
    return p.nul.valid() && SetHandleInformation(p.out_w.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
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

UniqueHandle low_token() noexcept
{
    HANDLE cur = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY, &cur))
        return {};
    const UniqueHandle own(cur);
    HANDLE dup = nullptr;
    if (!DuplicateTokenEx(own.get(), MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &dup))
        return {};
    UniqueHandle token(dup);
    PSID sid = nullptr;
    if (!ConvertStringSidToSidW(L"S-1-16-4096", &sid))
        return {};
    TOKEN_MANDATORY_LABEL tml{};
    tml.Label.Attributes = SE_GROUP_INTEGRITY;
    tml.Label.Sid = sid;
    const bool ok = SetTokenInformation(token.get(), TokenIntegrityLevel, &tml, sizeof tml + GetLengthSid(sid)) != 0;
    LocalFree(sid);
    if (!ok)
        token.reset();
    return token;
}

std::wstring env_block(const Cfg& c)
{
    wchar_t win[MAX_PATH];
    const UINT n = GetSystemWindowsDirectoryW(win, MAX_PATH);
    std::wstring b;
    auto add = [&](const std::wstring& entry) {
        b += entry;
        b.push_back(L'\0');
    };
    add(L"SystemRoot=" + std::wstring(win, n < MAX_PATH ? n : 0));
    add(L"TEMP=" + c.temp.wstring());
    add(L"TMP=" + c.temp.wstring());
    add(L"PATH=" + c.piper.wstring());
    b.push_back(L'\0');
    return b;
}

bool label_low(const fs::path& dir)
{
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"S:(ML;OICI;NW;;;LW)", SDDL_REVISION_1, &sd, nullptr))
        return false;
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL sacl = nullptr;
    std::wstring path = dir.wstring();
    const bool ok = GetSecurityDescriptorSacl(sd, &present, &sacl, &defaulted) && present
        && SetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, sacl)
            == ERROR_SUCCESS;
    LocalFree(sd);
    return ok;
}

void remove_stale(const fs::path& dir)
{
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::wstring name = it->path().filename().wstring();
        if (name.starts_with(L"eb-") && name.ends_with(L".wav"))
            fs::remove(it->path(), ec);
    }
}

}

bool is_reparse(const fs::path& p) noexcept
{
    const DWORD a = GetFileAttributesW(p.c_str());
    return a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool prepare_temp_dir(const fs::path& dir) noexcept
{
    try {
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (!is_plain_dir_chain(dir) || !label_low(dir))
            return false;
        remove_stale(dir);
        return true;
    } catch (...) {
        return false;
    }
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
    if (is_reparse(model) || is_reparse(json) || !fs::is_regular_file(model, ec) || !fs::is_regular_file(json, ec))
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

std::wstring voice_lang(const fs::path& model)
{
    fs::path json = model;
    json += L".json";
    std::wstring lang = lang_from_model_json(read_head(json));
    if (lang.empty()) {
        const std::wstring stem = model.stem().wstring();
        lang = stem.substr(0, stem.find(L'-'));
        std::ranges::replace(lang, L'_', L'-');
    }
    return lang;
}

Proc::~Proc()
{
    if (process_.valid())
        TerminateProcess(process_.get(), 1);
    if (reader_.joinable()) {
        CancelSynchronousIo(reader_.native_handle());
        reader_.join();
    }
    if (process_.valid())
        WaitForSingleObject(process_.get(), 1000);
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
        const UniqueHandle token = low_token();
        if (!token.valid() || !make_pipes(pp))
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
        std::wstring env = env_block(c);
        constexpr DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS
            | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;
        const fs::path exe = c.exe();
        if (!CreateProcessAsUserW(token.get(), exe.c_str(), line.data(), nullptr, nullptr, TRUE, flags, env.data(),
                c.piper.c_str(), &si.StartupInfo, &pi))
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

bool Proc::send(const std::string& line, const Cancel& cancel) noexcept
{
    try {
        const std::string data = line + "\n";
        const UniqueHandle done(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        OVERLAPPED ov{};
        ov.hEvent = done.get();
        if (!done.valid() || (!WriteFile(in_w_.get(), data.data(), static_cast<DWORD>(data.size()), nullptr, &ov)
                && GetLastError() != ERROR_IO_PENDING))
            return false;
        const auto deadline = std::chrono::steady_clock::now() + kWriteLimit;
        const auto stop = [&]() noexcept {
            try {
                return cancel() || std::chrono::steady_clock::now() > deadline;
            } catch (...) {
                return true; // the pending write must still be cancelled and drained before ov goes away
            }
        };
        while (WaitForSingleObject(done.get(), static_cast<DWORD>(kPoll.count())) == WAIT_TIMEOUT) {
            if (stop()) {
                CancelIoEx(in_w_.get(), &ov);
                break;
            }
        }
        DWORD written = 0;
        return GetOverlappedResult(in_w_.get(), &ov, &written, TRUE) && written == data.size();
    } catch (...) {
        return false;
    }
}

Wait Proc::await(const Cancel& cancel, std::chrono::seconds limit) noexcept
{
    try {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        std::unique_lock lk(m_);
        for (;;) {
            while (!lines_.empty()) {
                const std::string l = std::move(lines_.front());
                lines_.pop_front();
                if (l.ends_with(".wav"))
                    return Wait::Done;
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

}
