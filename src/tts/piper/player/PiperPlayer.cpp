#include "tts/piper/player/PiperPlayer.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Storage.h>

#include <algorithm>
#include <atomic>
#include <mutex>

namespace eb::piper {

using winrt::Windows::Media::Core::MediaSource;
using winrt::Windows::Media::Playback::MediaPlaybackState;
using winrt::Windows::Media::Playback::MediaPlayer;
using winrt::Windows::Storage::StorageFile;

namespace {

struct Hook {
    std::function<void(PlayEvent)> on_event;
    std::atomic<bool> started{false};

    void start() noexcept
    {
        if (!started.exchange(true))
            on_event(PlayEvent::Started);
    }
};

}

struct Player::Impl {
    std::mutex m;
    MediaPlayer mp{nullptr};
};

Apartment::Apartment() noexcept
{
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        ok_ = true;
    } catch (...) { // NOLINT(bugprone-empty-catch): ok() reports the failure
    }
}

Apartment::~Apartment()
{
    if (ok_)
        winrt::uninit_apartment();
}

bool Player::open(const std::filesystem::path& wav) noexcept
{
    try {
        auto impl = std::make_shared<Impl>();
        const StorageFile file = StorageFile::GetFileFromPathAsync(wav.wstring()).get();
        impl->mp = MediaPlayer();
        impl->mp.AutoPlay(false);
        impl->mp.Source(MediaSource::CreateFromStorageFile(file));
        impl_ = std::move(impl);
        return true;
    } catch (...) {
        return false;
    }
}

bool Player::play(double volume, std::function<void(PlayEvent)> on_event) noexcept
{
    try {
        const std::scoped_lock lk(impl_->m);
        auto h = std::make_shared<Hook>();
        h->on_event = std::move(on_event);
        auto& mp = impl_->mp;
        mp.Volume(std::clamp(volume, 0.0, 1.0));
        mp.MediaEnded([h](auto&&, auto&&) {
            h->start();
            h->on_event(PlayEvent::Finished);
        });
        mp.MediaFailed([h](auto&&, auto&&) { h->on_event(PlayEvent::Failed); });
        mp.PlaybackSession().PlaybackStateChanged([h](const auto& session, auto&&) {
            try {
                if (session.PlaybackState() == MediaPlaybackState::Playing)
                    h->start();
            } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
            }
        });
        mp.Play();
        return true;
    } catch (...) {
        return false;
    }
}

void Player::pause() noexcept
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

void Player::resume() noexcept
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

void Player::close() noexcept
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
