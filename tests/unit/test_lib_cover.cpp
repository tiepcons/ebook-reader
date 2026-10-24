#include "../integration/eb_test_util.h"
#include "base/ComScope.h"
#include "base/Hash.h"
#include "lib/CoverCache.h"
#include "test_base_tmp.h"

#include <windows.h>

#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>

namespace {

bool decoded_size(const fs::path& jpeg, UINT& w, UINT& h)
{
    Microsoft::WRL::ComPtr<IWICImagingFactory> f;
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> dec;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))))
        return false;
    if (FAILED(f->CreateDecoderFromFilename(jpeg.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)))
        return false;
    return SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&w, &h));
}

std::string id_of(const fs::path& book)
{
    auto id = eb::book_id(book);
    REQUIRE(id.has_value());
    return *id;
}

bool wait_for(eb::CoverWorker& w, size_t want, std::vector<eb::CoverResult>& got)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (got.size() < want && std::chrono::steady_clock::now() < end) {
        for (auto& r : w.take_ready(8))
            got.push_back(std::move(r));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return got.size() >= want;
}

}

TEST_CASE("cover cache: generates a 320 px JPEG for every good corpus format")
{
    const Lib lib;
    const eb::ComScope com(COINIT_MULTITHREADED);
    const ebtest::TempDir dir;
    const eb::CoverCache cache(dir.path());
    for (const auto& p : corpus("good")) {
        INFO(p.string());
        const std::string id = id_of(p);
        CHECK_FALSE(cache.cached(id).has_value());
        const auto out = cache.generate(id, p);
        REQUIRE(out.has_value());
        UINT w = 0, h = 0;
        REQUIRE(decoded_size(*out, w, h));
        CHECK(std::max(w, h) == 320);
        const auto hit = cache.cached(id);
        REQUIRE(hit.has_value());
        CHECK(*hit == *out);
    }
    for (const auto& e : fs::directory_iterator(dir.path()))
        CHECK(e.path().extension() == L".jpg");
}

TEST_CASE("cover cache: bad ids and bad books are rejected")
{
    const Lib lib;
    const ebtest::TempDir dir;
    const eb::CoverCache cache(dir.path());
    CHECK_FALSE(cache.cached("../evil").has_value());
    CHECK_FALSE(cache.cached("").has_value());
    CHECK_FALSE(cache.generate("../evil", first_with_ext(".epub")).has_value());
    CHECK_FALSE(cache.generate("abcdef", dir.path() / L"missing.epub").has_value());
    CHECK_FALSE(cache.cached("abcdef").has_value());
}

TEST_CASE("cover worker: serves requests and never retries the crash suspect")
{
    const Lib lib;
    const ebtest::TempDir dir;
    const fs::path covers = dir.path() / L"covers";
    fs::create_directories(covers);
    const fs::path marker = dir.path() / L"pending_cover";
    const fs::path good = first_with_ext(".epub");
    const fs::path other = first_with_ext(".pdf");
    const std::string good_id = id_of(good), suspect_id = id_of(other);
    std::ofstream(marker, std::ios::binary) << "cover\t" << suspect_id;

    std::atomic<int> signals{0};
    eb::CoverWorker worker(eb::CoverCache(covers), marker, [&] {
        ++signals;
        return true;
    });
    REQUIRE(worker.previous_suspect().has_value());
    CHECK(*worker.previous_suspect() == suspect_id);
    worker.request(suspect_id, other, 96, 144);
    worker.request(good_id, good, 96, 144);
    std::vector<eb::CoverResult> got;
    REQUIRE(wait_for(worker, 2, got));
    CHECK(signals.load() >= 1);
    for (const auto& r : got) {
        CHECK(r.ok == (r.id == good_id));
        if (r.ok) {
            CHECK(r.width == 96);
            CHECK(r.height == 144);
            REQUIRE(r.pixels.size() == 96U * 144U);
            CHECK((r.pixels[0] >> 24) == 0xFF);
        }
    }
    const eb::CoverCache cache(covers);
    CHECK(cache.cached(good_id).has_value());
    CHECK_FALSE(cache.cached(suspect_id).has_value());
    worker.stop();
    CHECK_FALSE(fs::exists(marker));
}
