#include "tts/winrt/WinRtPlayer.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>

#include <objbase.h>
#include <roapi.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <set>

namespace eb {

using winrt::Windows::Media::Core::IMediaCue;
using winrt::Windows::Media::Core::MediaSource;
using winrt::Windows::Media::Core::SpeechCue;
using winrt::Windows::Media::Playback::MediaPlaybackItem;
using winrt::Windows::Media::Playback::MediaPlaybackState;
using winrt::Windows::Media::Playback::MediaPlayer;
using winrt::Windows::Media::Playback::TimedMetadataTrackPresentationMode;
using winrt::Windows::Storage::StorageFile;
using winrt::Windows::Storage::Streams::DataWriter;
using winrt::Windows::Storage::Streams::InMemoryRandomAccessStream;
using winrt::Windows::Storage::Streams::IRandomAccessStream;

namespace {

// C++/WinRT falls back to LoadLibrary("<Namespace>.dll") from the app directory when a class is not registered;
// this handler keeps activation on RoGetActivationFactory only.
int32_t __stdcall activate_registered(void* class_id, winrt::guid const& iid, void** factory) noexcept
{
    const auto id = static_cast<HSTRING>(class_id);
    const auto& riid = reinterpret_cast<const IID&>(iid);
    const HRESULT hr = RoGetActivationFactory(id, riid, factory);
    if (hr != CO_E_NOTINITIALIZED)
        return hr;
    CO_MTA_USAGE_COOKIE cookie = nullptr;
    if (FAILED(CoIncrementMTAUsage(&cookie)))
        return hr;
    return RoGetActivationFactory(id, riid, factory);
}

[[maybe_unused]] const bool g_activation_pinned = (winrt_activation_handler = &activate_registered, true);

// Word boundaries come from the SpeechWord track's CueEntered event: it fires from the playback clock, so
// highlighting cannot drift from the audio, unlike polling PlaybackSession.Position on a timer.
struct Hook {
    WinRtPlayer::OnEvent on_event;
    std::atomic<bool> started{false};
    std::mutex m;
    std::set<uint32_t> hooked;

    void start() noexcept
    {
        if (!started.exchange(true))
            on_event(PlayEvent::Started, 0);
    }
    void word(int32_t pos) noexcept
    {
        if (pos < 0)
            return;
        start();
        on_event(PlayEvent::Word, static_cast<uint32_t>(pos));
    }
};

void on_cue(const std::shared_ptr<Hook>& h, const IMediaCue& c) noexcept
{
    try {
        const auto cue = c.try_as<SpeechCue>();
        if (!cue)
            return;
        const auto pos = cue.StartPositionInInput();
        if (pos)
            h->word(pos.Value());
    } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
    }
}

void hook_tracks(const std::shared_ptr<Hook>& h, const MediaPlaybackItem& item)
{
    const std::scoped_lock lk(h->m);
    auto tracks = item.TimedMetadataTracks();
    for (uint32_t i = 0; i < tracks.Size(); ++i) {
        if (!h->hooked.insert(i).second)
            continue;
        const auto track = tracks.GetAt(i);
        if (track.Id() != L"SpeechWord")
            continue;
        tracks.SetPresentationMode(i, TimedMetadataTrackPresentationMode::ApplicationPresented);
        track.CueEntered([h](auto&&, const auto& args) { on_cue(h, args.Cue()); });
    }
}

void hook_player(const MediaPlayer& mp, const MediaPlaybackItem& item, const std::shared_ptr<Hook>& h)
{
    mp.MediaEnded([h](auto&&, auto&&) {
        h->start();
        h->on_event(PlayEvent::Finished, 0);
    });
    mp.MediaFailed([h](auto&&, auto&&) { h->on_event(PlayEvent::Failed, 0); });
    mp.PlaybackSession().PlaybackStateChanged([h](const auto& session, auto&&) {
        try {
            if (session.PlaybackState() == MediaPlaybackState::Playing)
                h->start();
        } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
        }
    });
    item.TimedMetadataTracksChanged([h](const MediaPlaybackItem& it, auto&&) {
        try {
            hook_tracks(h, it);
        } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
        }
    });
    hook_tracks(h, item);
}

}

struct WinRtPlayer::Impl {
    std::mutex m;
    MediaPlayer mp;
    MediaPlaybackItem item{nullptr};

    explicit Impl(const MediaSource& src) : item(src)
    {
        mp.AutoPlay(false);
        mp.Source(item);
    }
};

WinRtApartment::WinRtApartment() noexcept
{
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        ok_ = true;
    } catch (...) { // NOLINT(bugprone-empty-catch): ok() reports the failure
    }
}

WinRtApartment::~WinRtApartment()
{
    if (ok_)
        winrt::uninit_apartment();
}

bool WinRtPlayer::open_file(const std::filesystem::path& wav) noexcept
{
    try {
        const StorageFile file = StorageFile::GetFileFromPathAsync(wav.wstring()).get();
        impl_ = std::make_shared<Impl>(MediaSource::CreateFromStorageFile(file));
        return true;
    } catch (...) {
        return false;
    }
}

bool WinRtPlayer::open_stream(IUnknown* stream, const wchar_t* mime) noexcept
{
    try {
        winrt::com_ptr<IUnknown> raw;
        raw.copy_from(stream);
        impl_ = std::make_shared<Impl>(MediaSource::CreateFromStream(raw.as<IRandomAccessStream>(), mime));
        return true;
    } catch (...) {
        return false;
    }
}

bool WinRtPlayer::open_bytes(std::span<const std::byte> audio, const wchar_t* mime) noexcept
{
    try {
        const InMemoryRandomAccessStream stream;
        const DataWriter writer(stream);
        writer.WriteBytes(winrt::array_view<const uint8_t>(
            reinterpret_cast<const uint8_t*>(audio.data()), static_cast<uint32_t>(audio.size())));
        writer.StoreAsync().get();
        writer.DetachStream();
        stream.Seek(0);
        impl_ = std::make_shared<Impl>(MediaSource::CreateFromStream(stream, mime));
        return true;
    } catch (...) {
        return false;
    }
}

bool WinRtPlayer::play(double volume, OnEvent on_event) noexcept
{
    try {
        const std::scoped_lock lk(impl_->m);
        auto h = std::make_shared<Hook>();
        h->on_event = std::move(on_event);
        impl_->mp.Volume(std::clamp(volume, 0.0, 1.0));
        hook_player(impl_->mp, impl_->item, h);
        impl_->mp.Play();
        return true;
    } catch (...) {
        return false;
    }
}

void WinRtPlayer::pause() noexcept
{
    if (!impl_)
        return;
    const std::scoped_lock lk(impl_->m);
    try {
        if (impl_->mp)
            impl_->mp.Pause();
    } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
    }
}

void WinRtPlayer::resume() noexcept
{
    if (!impl_)
        return;
    const std::scoped_lock lk(impl_->m);
    try {
        if (impl_->mp)
            impl_->mp.Play();
    } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
    }
}

void WinRtPlayer::close() noexcept
{
    if (!impl_)
        return;
    const std::scoped_lock lk(impl_->m);
    try {
        if (impl_->mp) {
            impl_->mp.Pause();
            impl_->mp.Close();
        }
    } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
    }
    impl_->mp = nullptr;
}

}
