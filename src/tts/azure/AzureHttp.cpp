#include "tts/azure/AzureHttp.h"

#include "base/Handle.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

namespace eb {
namespace {

constexpr int kResolveMs = 5000;
constexpr int kConnectMs = 5000;
constexpr int kIoMs = 15000;
constexpr DWORD kChunk = 32 * 1024;
constexpr DWORD kAwaitMs = 60000;
constexpr DWORD kClosingMs = 10000;
constexpr int kMaxRetryAfterDigits = 6;
constexpr std::wstring_view kKeyHeaderName = L"Ocp-Apim-Subscription-Key: ";

struct InetTraits {
    using Raw = HINTERNET;
    static bool valid(HINTERNET h) noexcept { return h != nullptr; }
    static void close(HINTERNET h) noexcept { WinHttpCloseHandle(h); }
};
using Inet = Unique<InetTraits>;

class ZeroWide {
public:
    explicit ZeroWide(size_t n) : buf_(new wchar_t[n + 1]()), cap_(n + 1) {}
    ZeroWide(const ZeroWide&) = delete;
    ZeroWide& operator=(const ZeroWide&) = delete;
    ~ZeroWide() { SecureZeroMemory(buf_.get(), cap_ * sizeof(wchar_t)); }
    wchar_t* data() noexcept { return buf_.get(); }

private:
    std::unique_ptr<wchar_t[]> buf_;
    size_t cap_;
};

// Per-request state shared with the WinHTTP callback thread. It lives until WinHTTP reports HANDLE_CLOSING.
struct Ctx {
    explicit Ctx(bool can_cancel)
        : done(CreateEventW(nullptr, FALSE, FALSE, nullptr)), cancel(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          closed(CreateEventW(nullptr, TRUE, FALSE, nullptr)), cancellable(can_cancel)
    {
    }
    [[nodiscard]] bool ok() const noexcept { return done && cancel && closed; }

    UniqueHandle done;   // an asynchronous operation finished
    UniqueHandle cancel; // set by Live::cancel / shutdown
    UniqueHandle closed; // HANDLE_CLOSING arrived
    std::string body;    // request body; WinHTTP may read it until SENDREQUEST_COMPLETE, so it lives (or leaks) with Ctx
    DWORD error = 1;
    DWORD got = 0;
    bool cancellable;
    std::vector<char> buf = std::vector<char>(kChunk);
};

void CALLBACK on_event(HINTERNET, DWORD_PTR context, DWORD status, void* info, DWORD len) noexcept
{
    auto* c = reinterpret_cast<Ctx*>(context); // NOLINT(performance-no-int-to-ptr): WinHTTP hands the context back as DWORD_PTR
    if (c == nullptr)
        return;
    switch (status) {
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
        c->error = info != nullptr ? static_cast<WINHTTP_ASYNC_RESULT*>(info)->dwError : 1;
        SetEvent(c->done.get());
        break;
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
        c->got = len;
        c->error = 0;
        SetEvent(c->done.get());
        break;
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
        c->error = 0;
        SetEvent(c->done.get());
        break;
    case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
        SetEvent(c->closed.get());
        break;
    default:
        break;
    }
}

class Live {
public:
    bool add(Ctx* c)
    {
        const std::scoped_lock lk(m_);
        if (shut_)
            return false;
        v_.push_back(c);
        return true;
    }
    void remove(Ctx* c) noexcept
    {
        const std::scoped_lock lk(m_);
        static_cast<void>(std::erase(v_, c));
    }
    void cancel(bool everything) noexcept
    {
        const std::scoped_lock lk(m_);
        for (Ctx* c : v_)
            if (everything || c->cancellable)
                SetEvent(c->cancel.get());
    }
    void shutdown() noexcept
    {
        {
            const std::scoped_lock lk(m_);
            shut_ = true;
        }
        cancel(true);
    }

private:
    std::mutex m_;
    std::vector<Ctx*> v_;
    bool shut_ = false;
};

// Owns the request handle: only the thread that runs the request ever closes it, so cancellation can never
// race with a call on the handle. The destructor closes the handle and waits for HANDLE_CLOSING.
class OwnedRequest {
public:
    OwnedRequest(Live& live, bool cancellable) : live_(&live), ctx_(std::make_unique<Ctx>(cancellable)) {}
    OwnedRequest(const OwnedRequest&) = delete;
    OwnedRequest& operator=(const OwnedRequest&) = delete;
    ~OwnedRequest()
    {
        if (registered_)
            live_->remove(ctx_.get());
        if (h_ != nullptr) {
            WinHttpCloseHandle(h_);
            if (WaitForSingleObject(ctx_->closed.get(), kClosingMs) != WAIT_OBJECT_0)
                ctx_.release(); // NOLINT(bugprone-unused-return-value): WinHTTP may still call back, so leak instead of free
        }
    }

    bool open(HINTERNET conn, const HttpRequest& r)
    {
        if (!ctx_->ok())
            return false;
        h_ = WinHttpOpenRequest(conn, r.post ? L"POST" : L"GET", r.path.c_str(), nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (h_ == nullptr)
            return false;
        const DWORD_PTR context = reinterpret_cast<DWORD_PTR>(ctx_.get());
        if (!WinHttpSetOption(h_, WINHTTP_OPTION_CONTEXT_VALUE, const_cast<DWORD_PTR*>(&context), sizeof context)) {
            WinHttpCloseHandle(h_);
            h_ = nullptr;
            return false;
        }
        DWORD no_redirect = WINHTTP_DISABLE_REDIRECTS;
        if (!WinHttpSetOption(h_, WINHTTP_OPTION_DISABLE_FEATURE, &no_redirect, sizeof no_redirect))
            return false;
        registered_ = live_->add(ctx_.get());
        return registered_;
    }
    [[nodiscard]] HINTERNET get() const noexcept { return h_; }
    [[nodiscard]] Ctx& ctx() const noexcept { return *ctx_; }

    // True when the operation started by the caller completed without error before cancel or timeout.
    [[nodiscard]] bool wait() const noexcept
    {
        const HANDLE both[2] = {ctx_->done.get(), ctx_->cancel.get()};
        return WaitForMultipleObjects(2, both, FALSE, kAwaitMs) == WAIT_OBJECT_0 && ctx_->error == 0;
    }

private:
    Live* live_;
    std::unique_ptr<Ctx> ctx_;
    HINTERNET h_ = nullptr;
    bool registered_ = false;
};

bool add_headers(HINTERNET req, const HttpRequest& r)
{
    if (!r.headers.empty()
        && !WinHttpAddRequestHeaders(req, r.headers.c_str(), static_cast<DWORD>(r.headers.size()), WINHTTP_ADDREQ_FLAG_ADD))
        return false;
    if (r.key == nullptr)
        return true;
    const auto key = r.key->view();
    ZeroWide line(kKeyHeaderName.size() + key.size());
    std::copy(kKeyHeaderName.begin(), kKeyHeaderName.end(), line.data());
    std::copy(key.begin(), key.end(), line.data() + kKeyHeaderName.size());
    return WinHttpAddRequestHeaders(req, line.data(), static_cast<DWORD>(kKeyHeaderName.size() + key.size()),
               WINHTTP_ADDREQ_FLAG_ADD)
        != FALSE;
}

bool issue(const OwnedRequest& req, const HttpRequest& r)
{
    if (!add_headers(req.get(), r))
        return false;
    Ctx& c = req.ctx();
    c.body = r.body;
    const auto len = static_cast<DWORD>(c.body.size());
    void* body = len == 0 ? WINHTTP_NO_REQUEST_DATA : c.body.data();
    if (!WinHttpSendRequest(req.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, body, len, len, reinterpret_cast<DWORD_PTR>(&req.ctx()))
        || !req.wait())
        return false;
    return WinHttpReceiveResponse(req.get(), nullptr) && req.wait();
}

int retry_after_seconds(HINTERNET req) noexcept
{
    wchar_t buf[16]{};
    DWORD size = sizeof buf;
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_RETRY_AFTER, WINHTTP_HEADER_NAME_BY_INDEX, buf, &size, WINHTTP_NO_HEADER_INDEX))
        return 0;
    int v = 0;
    int digits = 0;
    for (const wchar_t* p = buf; *p >= L'0' && *p <= L'9' && digits < kMaxRetryAfterDigits; ++p, ++digits)
        v = v * 10 + (*p - L'0');
    return v;
}

HttpResponse read_response(const OwnedRequest& req, const HttpRequest& r)
{
    HttpResponse out;
    DWORD status = 0;
    DWORD size = sizeof status;
    if (!WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
            &status, &size, WINHTTP_NO_HEADER_INDEX))
        return {};
    if (r.on_status)
        r.on_status(static_cast<int>(status));
    Ctx& c = req.ctx();
    for (;;) {
        if (!WinHttpReadData(req.get(), c.buf.data(), kChunk, nullptr) || !req.wait())
            return {};
        if (c.got == 0)
            break;
        if (out.body.size() + c.got > kAzureMaxResponse)
            return {};
        out.body.append(c.buf.data(), c.got);
    }
    out.status = static_cast<int>(status);
    if (status == 429)
        out.retry_after_s = retry_after_seconds(req.get());
    return out;
}

class WinHttpClient final : public HttpClient {
public:
    WinHttpClient() noexcept
    {
        session_.reset(WinHttpOpen(L"EbookReader", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
        if (!session_)
            return;
        constexpr DWORD kEvents = WINHTTP_CALLBACK_FLAG_SENDREQUEST_COMPLETE | WINHTTP_CALLBACK_FLAG_HEADERS_AVAILABLE
            | WINHTTP_CALLBACK_FLAG_READ_COMPLETE | WINHTTP_CALLBACK_FLAG_REQUEST_ERROR | WINHTTP_CALLBACK_FLAG_HANDLES;
        if (WinHttpSetStatusCallback(session_.get(), on_event, kEvents, 0) == WINHTTP_INVALID_STATUS_CALLBACK
            || !WinHttpSetTimeouts(session_.get(), kResolveMs, kConnectMs, kIoMs, kIoMs))
            session_.reset();
    }

    HttpResponse send(const HttpRequest& r) noexcept override
    {
        try {
            return do_send(r);
        } catch (...) {
            return {};
        }
    }
    void cancel() noexcept override { live_.cancel(false); }
    void shutdown() noexcept override { live_.shutdown(); }

private:
    HttpResponse do_send(const HttpRequest& r)
    {
        if (!session_)
            return {};
        const Inet conn(WinHttpConnect(session_.get(), r.host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
        if (!conn)
            return {};
        OwnedRequest req(live_, r.cancellable);
        if (!req.open(conn.get(), r) || !issue(req, r))
            return {};
        return read_response(req, r);
    }

    Inet session_;
    Live live_;
};

}

std::unique_ptr<HttpClient> make_winhttp_client() noexcept
{
    try {
        return std::make_unique<WinHttpClient>();
    } catch (...) {
        return nullptr;
    }
}

}
