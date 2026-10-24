#include "tts/azure/AzurePlayer.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <atomic>

namespace eb {

using winrt::Windows::Media::Core::MediaSource;
using winrt::Windows::Media::Playback::MediaPlaybackState;
using winrt::Windows::Media::Playback::MediaPlayer;
using winrt::Windows::Storage::Streams::DataWriter;
using winrt::Windows::Storage::Streams::InMemoryRandomAccessStream;

namespace {

struct Hook {
    std::shared_ptr<AzureSink> sink;
    uint32_t gen;
    uint32_t id;
    std::atomic<bool> started{false};

    void start() noexcept
    {
        if (!started.exchange(true))
            sink->post(gen, id, TtsEvent::Started, 0);
    }
};

void close_player(MediaPlayer& p) noexcept
{
    try {
        if (p) {
            p.Pause();
            p.Close();
        }
    } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
    }
    p = nullptr;
}

MediaPlayer build_player(const std::vector<std::byte>& audio)
{
    const InMemoryRandomAccessStream stream;
    const DataWriter writer(stream);
    writer.WriteBytes(winrt::array_view<const uint8_t>(
        reinterpret_cast<const uint8_t*>(audio.data()), static_cast<uint32_t>(audio.size())));
    writer.StoreAsync().get();
    writer.DetachStream();
    stream.Seek(0);
    MediaPlayer p;
    p.AutoPlay(false);
    p.Source(MediaSource::CreateFromStream(stream, L"audio/mpeg"));
    return p;
}

void hook_player(const MediaPlayer& p, const std::shared_ptr<Hook>& h)
{
    p.MediaEnded([h](auto&&, auto&&) {
        h->start();
        h->sink->post(h->gen, h->id, TtsEvent::Finished, 0);
    });
    p.MediaFailed([h](auto&&, auto&&) { h->sink->post(h->gen, h->id, TtsEvent::Error, kTtsPlayFailed); });
    p.PlaybackSession().PlaybackStateChanged([h](const auto& session, auto&&) {
        try {
            if (session.PlaybackState() == MediaPlaybackState::Playing)
                h->start();
        } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
        }
    });
}

}

struct AzurePlayer::Impl {
    std::shared_ptr<AzureSink> sink;
    MediaPlayer cur{nullptr};
    std::unique_ptr<AzureTaskThread> thread;

    void play(uint32_t gen, uint32_t id, const AzureAudio& audio, double volume)
    {
        if (!sink->current(gen))
            return;
        MediaPlayer p{nullptr};
        try {
            p = build_player(*audio);
            p.Volume(std::clamp(volume, 0.0, 1.0));
            hook_player(p, std::shared_ptr<Hook>(new Hook{sink, gen, id}));
        } catch (...) {
            close_player(p);
            sink->post(gen, id, TtsEvent::Error, kTtsPlayFailed);
            return;
        }
        close_player(cur);
        cur = p;
        try {
            cur.Play();
        } catch (...) {
            close_player(cur);
            sink->post(gen, id, TtsEvent::Error, kTtsPlayFailed);
        }
    }
};

AzurePlayer::AzurePlayer(std::shared_ptr<AzureSink> sink) : impl_(std::make_unique<Impl>())
{
    impl_->sink = std::move(sink);
    Impl* im = impl_.get();
    impl_->thread = std::make_unique<AzureTaskThread>(
        [] { winrt::init_apartment(winrt::apartment_type::multi_threaded); },
        [im] {
            close_player(im->cur);
            winrt::uninit_apartment();
        });
}

AzurePlayer::~AzurePlayer()
{
    impl_->thread.reset();
}

void AzurePlayer::play(uint32_t gen, uint32_t id, AzureAudio audio, double volume) noexcept
{
    impl_->thread->push([im = impl_.get(), gen, id, a = std::move(audio), volume] { im->play(gen, id, a, volume); });
}

void AzurePlayer::pause() noexcept
{
    impl_->thread->push([im = impl_.get()] {
        if (im->cur)
            im->cur.Pause();
    });
}

void AzurePlayer::resume() noexcept
{
    impl_->thread->push([im = impl_.get()] {
        if (im->cur)
            im->cur.Play();
    });
}

void AzurePlayer::stop() noexcept
{
    impl_->thread->push([im = impl_.get()] { close_player(im->cur); });
}

}
