#include "tts/azure/AzureHttp.h"

#include "base/Handle.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <mutex>
#include <vector>

namespace eb {
namespace {

constexpr int kResolveMs = 5000;
constexpr int kConnectMs = 5000;
constexpr int kIoMs = 15000;
constexpr DWORD kChunk = 32 * 1024;
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

class Live {
public:
    void add(HINTERNET h)
    {
        const std::scoped_lock lk(m_);
        v_.push_back(h);
    }
    bool remove(HINTERNET h) noexcept
    {
        const std::scoped_lock lk(m_);
        const auto it = std::find(v_.begin(), v_.end(), h);
        if (it == v_.end())
            return false;
        v_.erase(it);
        return true;
    }
    void cancel_all() noexcept
    {
        const std::scoped_lock lk(m_);
        for (HINTERNET h : v_)
            WinHttpCloseHandle(h);
        v_.clear();
    }

private:
    std::mutex m_;
    std::vector<HINTERNET> v_;
};

class OwnedRequest {
public:
    OwnedRequest(Live& live, bool cancellable) noexcept : live_(&live), cancellable_(cancellable) {}
    OwnedRequest(const OwnedRequest&) = delete;
    OwnedRequest& operator=(const OwnedRequest&) = delete;
    ~OwnedRequest()
    {
        if (h_ == nullptr)
            return;
        if (!cancellable_ || live_->remove(h_))
            WinHttpCloseHandle(h_);
    }

    bool open(HINTERNET conn, const HttpRequest& r)
    {
        h_ = WinHttpOpenRequest(conn, r.post ? L"POST" : L"GET", r.path.c_str(), nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (h_ == nullptr)
            return false;
        DWORD no_redirect = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(h_, WINHTTP_OPTION_DISABLE_FEATURE, &no_redirect, sizeof no_redirect);
        if (cancellable_) {
            try {
                live_->add(h_);
            } catch (...) {
                WinHttpCloseHandle(h_);
                h_ = nullptr;
                return false;
            }
        }
        return true;
    }
    [[nodiscard]] HINTERNET get() const noexcept { return h_; }

private:
    Live* live_;
    bool cancellable_;
    HINTERNET h_ = nullptr;
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

bool issue(HINTERNET req, const HttpRequest& r)
{
    if (!add_headers(req, r))
        return false;
    const auto len = static_cast<DWORD>(r.body.size());
    void* body = len == 0 ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(r.body.data());
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, body, len, len, 0))
        return false;
    return WinHttpReceiveResponse(req, nullptr) != FALSE;
}

HttpResponse read_response(HINTERNET req)
{
    HttpResponse out;
    DWORD status = 0;
    DWORD size = sizeof status;
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
            &status, &size, WINHTTP_NO_HEADER_INDEX))
        return {};
    std::vector<char> buf(kChunk);
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(req, buf.data(), kChunk, &got))
            return {};
        if (got == 0)
            break;
        if (out.body.size() + got > kAzureMaxResponse)
            return {};
        out.body.append(buf.data(), got);
    }
    out.status = static_cast<int>(status);
    return out;
}

class WinHttpClient final : public HttpClient {
public:
    WinHttpClient() noexcept
    {
        session_.reset(WinHttpOpen(L"EbookReader", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0));
        if (session_)
            WinHttpSetTimeouts(session_.get(), kResolveMs, kConnectMs, kIoMs, kIoMs);
    }

    HttpResponse send(const HttpRequest& r) noexcept override
    {
        try {
            return do_send(r);
        } catch (...) {
            return {};
        }
    }
    void cancel() noexcept override { live_.cancel_all(); }

private:
    HttpResponse do_send(const HttpRequest& r)
    {
        if (!session_)
            return {};
        const Inet conn(WinHttpConnect(session_.get(), r.host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
        if (!conn)
            return {};
        OwnedRequest req(live_, r.cancellable);
        if (!req.open(conn.get(), r) || !issue(req.get(), r))
            return {};
        return read_response(req.get());
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
