#include "tts/azure/AzureProbe.h"

#include "base/Log.h"
#include "tts/azure/AzureProto.h"

#include <utility>

namespace eb {
namespace {

constexpr std::wstring_view kHostSuffix = L".tts.speech.microsoft.com";
constexpr std::wstring_view kVoicesPath = L"/cognitiveservices/voices/list";

}

AzureProbe::~AzureProbe()
{
    if (http_)
        http_->shutdown();
    if (worker_.joinable())
        worker_.join();
}

bool AzureProbe::start(std::string region, SecretString key, std::function<void(int)> done) noexcept
{
    try {
        if (!http_ || !done || !valid_azure_region(region) || busy_.exchange(true))
            return false;
        if (worker_.joinable())
            worker_.join();
        worker_ = std::thread([this, region = std::move(region), key = std::move(key), done = std::move(done)] {
            HttpRequest rq;
            rq.host = std::wstring(region.begin(), region.end()) + std::wstring(kHostSuffix);
            rq.path = std::wstring(kVoicesPath);
            rq.headers = L"User-Agent: EbookReader";
            rq.key = &key;
            const int status = http_->send(rq).status;
            busy_ = false;
            try {
                done(status);
            } catch (...) {
                log::error("azure probe: callback failed");
            }
        });
        return true;
    } catch (...) {
        busy_ = false;
        return false;
    }
}

}
