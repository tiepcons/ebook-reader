#include "CoverCache.h"

#include "Metadata.h"
#include "base/AtomicFile.h"
#include "base/ComScope.h"
#include "base/Log.h"

#include <windows.h>

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

namespace eb {
namespace {

using Microsoft::WRL::ComPtr;

constexpr float kJpegQuality = 0.85F;
constexpr size_t kMaxIdChars = 128;
constexpr uint32_t kCellBg = 0xFFEBEBEB;
constexpr UINT kMaxSidePx = 16384;
constexpr int kMinCell = 16, kMaxCell = 2048;

struct BitmapFree {
    void operator()(eb_bitmap* b) const noexcept { eb_bitmap_free(b); }
};
using UniqueBitmap = std::unique_ptr<eb_bitmap, BitmapFree>;

Error wic_error(const wchar_t* what)
{
    return Error{ErrorCode::Internal, what};
}

Result<ComPtr<IWICImagingFactory>> make_factory()
{
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))))
        return std::unexpected(wic_error(L"wic factory"));
    return f;
}

// Premultiplied BGRA over white, as 24bpp BGR.
std::vector<uint8_t> flatten_on_white(const eb_bitmap& b)
{
    const auto w = static_cast<size_t>(b.width);
    std::vector<uint8_t> out(w * static_cast<size_t>(b.height) * 3);
    for (size_t y = 0; y < static_cast<size_t>(b.height); ++y) {
        const uint8_t* src = b.bgra + y * static_cast<size_t>(b.stride);
        uint8_t* dst = out.data() + y * w * 3;
        for (size_t x = 0; x < w; ++x, src += 4, dst += 3) {
            const int white = 255 - src[3];
            for (int c = 0; c < 3; ++c)
                dst[c] = static_cast<uint8_t>(std::min(255, src[c] + white));
        }
    }
    return out;
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

Result<std::vector<std::byte>> encode_jpeg(IWICImagingFactory* f, const eb_bitmap& b)
{
    if (b.width <= 0 || b.height <= 0 || b.stride < b.width * 4 || b.bgra == nullptr)
        return fail(ErrorCode::Format, L"bad cover bitmap");
    const std::vector<uint8_t> bgr = flatten_on_white(b);
    ComPtr<IWICBitmap> bmp;
    if (FAILED(f->CreateBitmapFromMemory(static_cast<UINT>(b.width), static_cast<UINT>(b.height),
            GUID_WICPixelFormat24bppBGR, static_cast<UINT>(b.width) * 3, static_cast<UINT>(bgr.size()),
            const_cast<BYTE*>(bgr.data()), &bmp)))
        return std::unexpected(wic_error(L"wic bitmap"));
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)))
        return fail(ErrorCode::NoMemory);
    if (auto r = encode_frame(f, stream.Get(), bmp.Get(), b); !r)
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

ComPtr<IWICBitmapSource> scaled_source(IWICImagingFactory* f, const std::filesystem::path& file, int w, int h, int* nw, int* nh)
{
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    UINT sw = 0, sh = 0;
    if (FAILED(f->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))
        || FAILED(dec->GetFrame(0, &frame)) || FAILED(frame->GetSize(&sw, &sh)) || sw == 0 || sh == 0 || sw > kMaxSidePx
        || sh > kMaxSidePx)
        return nullptr;
    const double s = std::min(static_cast<double>(w) / sw, static_cast<double>(h) / sh);
    *nw = std::max(1, static_cast<int>(std::lround(sw * s)));
    *nh = std::max(1, static_cast<int>(std::lround(sh * s)));
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> conv;
    ComPtr<IWICBitmapSource> out;
    if (FAILED(f->CreateBitmapScaler(&scaler))
        || FAILED(scaler->Initialize(frame.Get(), static_cast<UINT>(*nw), static_cast<UINT>(*nh),
            WICBitmapInterpolationModeHighQualityCubic))
        || FAILED(f->CreateFormatConverter(&conv))
        || FAILED(conv->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeCustom)))
        return nullptr;
    conv.As(&out);
    return out;
}

// The finished list cell: the JPEG scaled to fit w x h, centred on a neutral background.
Result<std::vector<uint32_t>> render_cell(IWICImagingFactory* f, const std::filesystem::path& jpeg, int w, int h)
{
    int nw = 0, nh = 0;
    const auto src = scaled_source(f, jpeg, w, h, &nw, &nh);
    if (!src)
        return fail(ErrorCode::Format, L"cover decode failed");
    std::vector<uint32_t> cell(static_cast<size_t>(w) * static_cast<size_t>(h), kCellBg);
    const UINT stride = static_cast<UINT>(w) * 4;
    const size_t offset = static_cast<size_t>((h - nh) / 2) * stride + static_cast<size_t>((w - nw) / 2) * 4;
    const UINT room = stride * static_cast<UINT>(h) - static_cast<UINT>(offset);
    if (FAILED(src->CopyPixels(nullptr, stride, room, reinterpret_cast<BYTE*>(cell.data()) + offset)))
        return fail(ErrorCode::Format, L"cover copy failed");
    return cell;
}

bool valid_id(std::string_view id) noexcept
{
    return !id.empty() && id.size() <= kMaxIdChars
        && std::ranges::all_of(id, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

struct AbortScope {
    AbortSlot* slot;
    AbortScope(AbortSlot* s, eb_doc* doc) noexcept : slot(s)
    {
        if (slot != nullptr)
            slot->set(doc);
    }
    AbortScope(const AbortScope&) = delete;
    AbortScope& operator=(const AbortScope&) = delete;
    ~AbortScope()
    {
        if (slot != nullptr)
            slot->set(nullptr);
    }
};

} // namespace

void AbortSlot::set(eb_doc* doc) noexcept
{
    const std::scoped_lock lock(mu_);
    doc_ = doc;
}

void AbortSlot::abort() noexcept
{
    const std::scoped_lock lock(mu_);
    if (doc_ != nullptr)
        eb_abort(doc_);
}

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

Result<std::filesystem::path> CoverCache::generate(std::string_view id, const std::filesystem::path& book,
    IWICImagingFactory* wic, AbortSlot* abort, const std::stop_token& stop) const noexcept
{
    try {
        const auto target = path_for(id);
        if (!target)
            return fail(ErrorCode::Format, L"bad book id");
        ComPtr<IWICImagingFactory> own;
        if (wic == nullptr) {
            auto f = make_factory();
            if (!f)
                return std::unexpected(std::move(f.error()));
            own = *f;
            wic = own.Get();
        }
        auto doc = open_book(book);
        if (!doc)
            return std::unexpected(std::move(doc.error()));
        eb_bitmap* raw = nullptr;
        {
            const AbortScope scope(abort, doc->get());
            if (stop.stop_requested())
                return fail(ErrorCode::Unavailable, L"cover cancelled");
            if (const eb_status st = eb_render_cover(doc->get(), kCoverSidePx, &raw); st != EB_OK)
                return fail(error_from_status(st), L"cover render failed");
        }
        const UniqueBitmap bmp(raw);
        auto jpeg = encode_jpeg(wic, *bmp);
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

void CoverWorker::signal(bool call) noexcept
{
    if (!call || !notify_)
        return;
    if (!notify_()) {
        const std::scoped_lock lock(mu_);
        notified_ = false;
    }
}

std::optional<std::string> CoverWorker::request(const std::string& id, const std::filesystem::path& book, int cell_w, int cell_h)
{
    std::optional<std::string> dropped;
    bool retry = false;
    {
        const std::scoped_lock lock(mu_);
        std::erase_if(queue_, [&](const Job& j) { return j.id == id; });
        if (queue_.size() >= kMaxPendingCovers) {
            dropped = std::move(queue_.front().id);
            queue_.erase(queue_.begin());
        }
        queue_.push_back({id, book, cell_w, cell_h});
        retry = !ready_.empty() && !notified_;
        notified_ = notified_ || retry;
    }
    cv_.notify_one();
    signal(retry);
    return dropped;
}

std::vector<CoverResult> CoverWorker::take_ready(size_t max)
{
    std::vector<CoverResult> out;
    {
        const std::scoped_lock lock(mu_);
        const auto n = static_cast<std::ptrdiff_t>(std::min(max, ready_.size()));
        out.assign(std::make_move_iterator(ready_.begin()), std::make_move_iterator(ready_.begin() + n));
        ready_.erase(ready_.begin(), ready_.begin() + n);
        if (ready_.empty())
            notified_ = false;
    }
    cv_.notify_all();
    return out;
}

void CoverWorker::stop() noexcept
{
    thread_.request_stop();
    abort_.abort();
    cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

void CoverWorker::process(const Job& job, IWICImagingFactory* wic, CoverResult& r, const std::stop_token& stop) noexcept
{
    try {
        if ((suspect_ && *suspect_ == job.id) || job.w < kMinCell || job.h < kMinCell || job.w > kMaxCell || job.h > kMaxCell)
            return;
        auto file = cache_.cached(job.id);
        if (!file) {
            const CrashLoopGuard::Scope scope(guard_, "cover", job.id);
            auto gen = cache_.generate(job.id, job.book, wic, &abort_, stop);
            if (!gen) {
                log::error("cover generation failed (code {})", static_cast<int>(gen.error().code));
                return;
            }
            file = std::move(*gen);
        }
        auto cell = render_cell(wic, *file, job.w, job.h);
        if (cell) {
            r.pixels = std::move(*cell);
            r.ok = true;
        }
    } catch (...) {
        r.ok = false;
    }
}

void CoverWorker::run(const std::stop_token& stop) noexcept
{
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    const ComScope com(COINIT_MULTITHREADED);
    try {
        auto wic = make_factory();
        if (!wic) {
            log::error("cover worker: WIC unavailable");
            return;
        }
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mu_);
                cv_.wait(lock, stop, [&] { return !queue_.empty() && ready_.size() < kMaxReadyCovers; });
                if (stop.stop_requested())
                    return;
                job = std::move(queue_.back());
                queue_.pop_back();
            }
            CoverResult result{job.id, false, job.w, job.h, {}};
            process(job, wic->Get(), result, stop);
            bool call = false;
            {
                const std::scoped_lock lock(mu_);
                ready_.push_back(std::move(result));
                call = !notified_;
                notified_ = true;
            }
            signal(call);
        }
    } catch (...) {
        log::error("cover worker stopped by an exception");
    }
}

}
