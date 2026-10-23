#include "CoverCache.h"

#include "Metadata.h"
#include "base/AtomicFile.h"
#include "base/Log.h"

#include <windows.h>

#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <iterator>

namespace eb {
namespace {

using Microsoft::WRL::ComPtr;

constexpr float kJpegQuality = 0.85F;
constexpr size_t kMaxIdChars = 128;

struct ComScope {
    bool owns;
    ComScope() noexcept : owns(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    ~ComScope()
    {
        if (owns)
            CoUninitialize();
    }
};

struct BitmapFree {
    void operator()(eb_bitmap* b) const noexcept { eb_bitmap_free(b); }
};
using UniqueBitmap = std::unique_ptr<eb_bitmap, BitmapFree>;

Error wic_error(const wchar_t* what)
{
    return Error{ErrorCode::Internal, what};
}

Result<ComPtr<IWICBitmapSource>> to_bgr24(IWICImagingFactory* f, const eb_bitmap& b)
{
    ComPtr<IWICBitmap> bmp;
    const UINT size = static_cast<UINT>(b.stride) * static_cast<UINT>(b.height);
    if (FAILED(f->CreateBitmapFromMemory(static_cast<UINT>(b.width), static_cast<UINT>(b.height),
            GUID_WICPixelFormat32bppPBGRA, static_cast<UINT>(b.stride), size, b.bgra, &bmp)))
        return std::unexpected(wic_error(L"wic bitmap"));
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(f->CreateFormatConverter(&conv))
        || FAILED(conv->Initialize(bmp.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeCustom)))
        return std::unexpected(wic_error(L"wic convert"));
    ComPtr<IWICBitmapSource> src;
    conv.As(&src);
    return src;
}

Result<void> encode_frame(IWICImagingFactory* f, IStream* stream, IWICBitmapSource* src, const eb_bitmap& b)
{
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(f->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc))
        || FAILED(enc->Initialize(stream, WICBitmapEncoderNoCache)) || FAILED(enc->CreateNewFrame(&frame, &props)))
        return std::unexpected(wic_error(L"jpeg encoder"));
    PROPBAG2 opt{};
    opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_R4;
    v.fltVal = kJpegQuality;
    WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
    if (FAILED(props->Write(1, &opt, &v)) || FAILED(frame->Initialize(props.Get()))
        || FAILED(frame->SetSize(static_cast<UINT>(b.width), static_cast<UINT>(b.height)))
        || FAILED(frame->SetPixelFormat(&pf)) || FAILED(frame->WriteSource(src, nullptr)) || FAILED(frame->Commit())
        || FAILED(enc->Commit()))
        return std::unexpected(wic_error(L"jpeg write"));
    return {};
}

Result<std::vector<std::byte>> encode_jpeg(const eb_bitmap& b)
{
    if (b.width <= 0 || b.height <= 0 || b.stride < b.width * 4 || b.bgra == nullptr)
        return fail(ErrorCode::Format, L"bad cover bitmap");
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))))
        return std::unexpected(wic_error(L"wic factory"));
    auto src = to_bgr24(f.Get(), b);
    if (!src)
        return std::unexpected(std::move(src.error()));
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)))
        return fail(ErrorCode::NoMemory);
    if (auto r = encode_frame(f.Get(), stream.Get(), src->Get(), b); !r)
        return std::unexpected(std::move(r.error()));
    STATSTG stat{};
    HGLOBAL mem = nullptr;
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || FAILED(GetHGlobalFromStream(stream.Get(), &mem)))
        return std::unexpected(wic_error(L"jpeg stream"));
    const void* p = GlobalLock(mem);
    if (p == nullptr)
        return std::unexpected(wic_error(L"jpeg lock"));
    std::vector<std::byte> out(static_cast<size_t>(stat.cbSize.QuadPart));
    std::memcpy(out.data(), p, out.size());
    GlobalUnlock(mem);
    return out;
}

bool valid_id(std::string_view id) noexcept
{
    return !id.empty() && id.size() <= kMaxIdChars
        && std::ranges::all_of(id, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

} // namespace

std::optional<std::filesystem::path> CoverCache::path_for(std::string_view id) const noexcept
{
    if (!valid_id(id))
        return std::nullopt;
    try {
        return dir_ / (std::wstring(id.begin(), id.end()) + L".jpg");
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::filesystem::path> CoverCache::cached(std::string_view id) const noexcept
{
    auto p = path_for(id);
    if (!p)
        return std::nullopt;
    std::error_code ec;
    if (std::filesystem::is_regular_file(*p, ec) && !ec)
        return p;
    return std::nullopt;
}

Result<std::filesystem::path> CoverCache::generate(std::string_view id, const std::filesystem::path& book) const noexcept
{
    try {
        const auto target = path_for(id);
        if (!target)
            return fail(ErrorCode::Format, L"bad book id");
        const ComScope com;
        auto doc = open_book(book);
        if (!doc)
            return std::unexpected(std::move(doc.error()));
        eb_bitmap* raw = nullptr;
        if (const eb_status st = eb_render_cover(doc->get(), kCoverSidePx, &raw); st != EB_OK)
            return fail(error_from_status(st), L"cover render failed");
        const UniqueBitmap bmp(raw);
        auto jpeg = encode_jpeg(*bmp);
        if (!jpeg)
            return std::unexpected(std::move(jpeg.error()));
        if (auto w = write_atomic(*target, *jpeg); !w)
            return std::unexpected(std::move(w.error()));
        return *target;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::NoMemory);
    } catch (...) {
        return fail(ErrorCode::Internal);
    }
}

CoverWorker::CoverWorker(CoverCache cache, std::filesystem::path marker_file, std::function<bool()> notify)
    : cache_(std::move(cache)), guard_(std::move(marker_file)), suspect_(guard_.previous_suspect()), notify_(std::move(notify))
{
    thread_ = std::jthread([this](const std::stop_token& st) { run(st); });
}

std::optional<std::string> CoverWorker::request(const std::string& id, const std::filesystem::path& book)
{
    std::optional<std::string> dropped;
    {
        const std::scoped_lock lock(mu_);
        std::erase_if(queue_, [&](const Job& j) { return j.id == id; });
        if (queue_.size() >= kMaxPendingCovers) {
            dropped = std::move(queue_.front().id);
            queue_.erase(queue_.begin());
        }
        queue_.push_back({id, book});
    }
    cv_.notify_one();
    return dropped;
}

std::vector<CoverResult> CoverWorker::take_ready(size_t max)
{
    const std::scoped_lock lock(mu_);
    const auto n = static_cast<std::ptrdiff_t>(std::min(max, ready_.size()));
    std::vector<CoverResult> out(std::make_move_iterator(ready_.begin()), std::make_move_iterator(ready_.begin() + n));
    ready_.erase(ready_.begin(), ready_.begin() + n);
    if (ready_.empty())
        notified_ = false;
    return out;
}

void CoverWorker::stop() noexcept
{
    thread_.request_stop();
    cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

bool CoverWorker::process(const Job& job) noexcept
{
    if (suspect_ && *suspect_ == job.id)
        return false;
    if (cache_.cached(job.id))
        return true;
    const CrashLoopGuard::Scope scope(guard_, "cover", job.id);
    const auto r = cache_.generate(job.id, job.book);
    if (!r)
        log::error("cover generation failed (code {})", static_cast<int>(r.error().code));
    return r.has_value();
}

void CoverWorker::run(const std::stop_token& stop) noexcept
{
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    try {
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mu_);
                cv_.wait(lock, stop, [&] { return !queue_.empty(); });
                if (stop.stop_requested())
                    return;
                job = std::move(queue_.back());
                queue_.pop_back();
            }
            const bool ok = process(job);
            bool call = false;
            {
                const std::scoped_lock lock(mu_);
                ready_.push_back({std::move(job.id), ok});
                call = !notified_;
                notified_ = true;
            }
            if (call && notify_ && !notify_()) {
                const std::scoped_lock lock(mu_);
                notified_ = false;
            }
        }
    } catch (...) {
        log::error("cover worker stopped by an exception");
    }
}

}
