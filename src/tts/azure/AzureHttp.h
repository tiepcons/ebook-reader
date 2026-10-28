#pragma once
#include "base/SecretStore.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace eb {

inline constexpr size_t kAzureMaxResponse = size_t{4} * 1024 * 1024;

struct HttpRequest {
    bool post = false;
    bool cancellable = true;
    std::wstring host;
    std::wstring path;
    std::wstring headers; // CRLF separated, never secret
    std::string body;
    const SecretString* key = nullptr; // sent as Ocp-Apim-Subscription-Key; never copied into a plain string
    std::function<void(int)> on_status; // called with the HTTP status once the headers arrived, before the body is read
};

// status == 0: no HTTP answer (DNS, connect, TLS, timeout, cancelled, body over kAzureMaxResponse).
struct HttpResponse {
    int status = 0;
    std::string body;
    int retry_after_s = 0; // Retry-After in seconds (429 only), 0 when absent or not a number
};

class HttpClient {
public:
    virtual ~HttpClient() = default;
    virtual HttpResponse send(const HttpRequest& req) noexcept = 0;
    // Aborts every in-flight request that was sent with cancellable == true.
    virtual void cancel() noexcept = 0;
    // Aborts every in-flight request and makes every later send() fail at once.
    virtual void shutdown() noexcept = 0;
};

// HTTPS only, asynchronous WinHTTP (only the calling thread ever closes a request handle), default certificate validation, redirects disabled, 5 s resolve/connect and 15 s send/receive timeouts.
std::unique_ptr<HttpClient> make_winhttp_client() noexcept;

}
