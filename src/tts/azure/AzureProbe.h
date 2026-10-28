#pragma once
#include "base/SecretStore.h"
#include "tts/azure/AzureHttp.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace eb {

// One asynchronous GET voices/list to check a key and a region. `done` runs on the worker thread with the HTTP
// status (0 = no answer). The destructor aborts the request and joins the worker.
class AzureProbe {
public:
    explicit AzureProbe(std::unique_ptr<HttpClient> http = make_winhttp_client()) noexcept : http_(std::move(http)) {}
    AzureProbe(const AzureProbe&) = delete;
    AzureProbe& operator=(const AzureProbe&) = delete;
    ~AzureProbe();

    // False when a probe is running, the region is invalid or no thread could be started.
    bool start(std::string region, SecretString key, std::function<void(int)> done) noexcept;

private:
    std::unique_ptr<HttpClient> http_;
    std::thread worker_;
    std::atomic<bool> busy_{false};
};

}
