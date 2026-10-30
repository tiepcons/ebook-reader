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
#include <chrono>
#include <chrono>
#include <cmath>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

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

// Playback starts only once the stream is open: starting while it is still opening lets the endpoint swallow the
// first words.
struct Gate {
    std::mutex m;
    bool opened = false, want_play = false;
    double rate = 1.0;
};

struct WinRtPlayer::Impl {
    std::mutex m;
    MediaPlayer mp;
    MediaPlaybackItem item{nullptr};
    std::shared_ptr<Gate> gate = std::make_shared<Gate>();

    explicit Impl(const MediaSource& src) : item(src)
    {
        mp.AutoPlay(false);
        mp.MediaOpened([g = gate](const MediaPlayer& sender, auto&&) {
            try {
                bool go = false;
                double rate = 1.0;
                {
                    const std::scoped_lock lk(g->m);
                    g->opened = true;
                    go = std::exchange(g->want_play, false);
                    rate = g->rate;
                }
                if (!go)
                    return;
                if (std::abs(rate - 1.0) > 1e-3)
                    sender.PlaybackSession().PlaybackRate(rate);
                sender.Play();
            } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
            }
        });
        mp.Source(item);
    }

    void play_when_open(double rate)
    {
        const std::scoped_lock lk(gate->m);
        gate->rate = rate;
        if (gate->opened)
            mp.Play();
        else
            gate->want_play = true;
    }

    // A pause or close before the stream opened must cancel the pending start.
    void cancel_pending(bool closing)
    {
        const std::scoped_lock lk(gate->m);
        gate->want_play = false;
        if (closing)
            gate->opened = false;
    }
};


struct KeepState {
    // Linger keeps playing for kLinger after a pause or stop, so a quick resume finds the endpoint awake, then pauses.
    enum Mode : uint8_t { Play, Pause, Linger, Done };
    static constexpr std::chrono::seconds kLinger{60};

    void set(Mode next)
    {
        {
            const std::scoped_lock lk(m);
            if (mode != Done)
                mode = next;
        }
        cv.notify_all();
    }
    [[nodiscard]] bool mode_is(Mode x)
    {
        const std::scoped_lock lk(m);
        return mode == x;
    }
    // Blocks until the mode differs from applied (or is Done) and returns it.
    Mode wait_change(Mode applied)
    {
        std::unique_lock lk(m);
        const auto changed = [&] { return mode != applied || mode == Done; };
        if (applied != Linger)
            cv.wait(lk, changed);
        else if (!cv.wait_for(lk, kLinger, changed))
            mode = Pause;
        return mode;
    }

    std::mutex m;
    std::condition_variable cv;
    Mode mode = Play;
};

namespace {

constexpr uint32_t kKeepRate = 48000;

std::atomic<int> g_workers{0};

void put32(std::vector<uint8_t>& v, uint32_t x)
{
    for (int i = 0; i < 4; ++i)
        v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}

void put16(std::vector<uint8_t>& v, uint16_t x)
{
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}

void put_tag(std::vector<uint8_t>& v, const char* tag)
{
    v.insert(v.end(), tag, tag + 4);
}

// One second of 16-bit mono PCM silence.
std::vector<uint8_t> silence_wav()
{
    const uint32_t data = kKeepRate * 2;
    std::vector<uint8_t> v;
    v.reserve(44 + data);
    put_tag(v, "RIFF");
    put32(v, 36 + data);
    put_tag(v, "WAVE");
    put_tag(v, "fmt ");
    put32(v, 16);
    put16(v, 1);
    put16(v, 1);
    put32(v, kKeepRate);
    put32(v, kKeepRate * 2);
    put16(v, 2);
    put16(v, 16);
    put_tag(v, "data");
    put32(v, data);
    v.resize(44 + data, 0);
    return v;
}

}

AudioKeepAlive::AudioKeepAlive(bool audible) noexcept : audible_(audible) {}

AudioKeepAlive::~AudioKeepAlive()
{
    stop();
    for (int i = 0; i < 100 && g_workers.load() > 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

// Runs on its own MTA thread: the setup awaits WinRT operations, which would block (and assert) on the UI thread.
// It follows the requested mode until Done and checks it between setup steps so teardown stays quick.
void AudioKeepAlive::run(const std::shared_ptr<KeepState>& s) noexcept
{
    try {
        const WinRtApartment apartment;
        if (!apartment.ok() || s->mode_is(KeepState::Done))
            return;
        const auto wav = silence_wav();
        const InMemoryRandomAccessStream stream;
        const DataWriter writer(stream);
        writer.WriteBytes(wav);
        writer.StoreAsync().get();
        writer.DetachStream();
        stream.Seek(0);
        if (s->mode_is(KeepState::Done))
            return;
        const MediaPlayer mp;
        mp.AutoPlay(false);
        mp.IsLoopingEnabled(true);
        mp.Volume(1.0);
        mp.Source(MediaSource::CreateFromStream(stream, L"audio/wav"));
        KeepState::Mode applied = KeepState::Done;
        for (;;) {
            const auto m = s->wait_change(applied);
            if (m == KeepState::Done)
                break;
            applied = m;
            if (m == KeepState::Play)
                mp.Play();
            else if (m == KeepState::Pause)
                mp.Pause();
        }
        mp.Pause();
        mp.Close();
    } catch (...) { // NOLINT(bugprone-empty-catch): no keep-alive is acceptable
    }
}

void AudioKeepAlive::start() noexcept
{
    wanted_ = true;
    if (!audible_)
        return;
    if (st_) {
        st_->set(KeepState::Play);
        return;
    }
    try {
        st_ = std::make_shared<KeepState>();
        ++g_workers;
        std::thread([s = st_] {
            run(s);
            --g_workers;
        }).detach();
    } catch (...) {
        --g_workers;
        st_.reset();
    }
}

void AudioKeepAlive::pause() noexcept
{
    const bool was = std::exchange(wanted_, false);
    if (st_ && was)
        st_->set(KeepState::Linger);
}

void AudioKeepAlive::stop() noexcept
{
    wanted_ = false;
    if (st_)
        st_->set(KeepState::Done);
    st_.reset();
}

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
        adopt();
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
        adopt();
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
        adopt();
        return true;
    } catch (...) {
        return false;
    }
}

namespace {

std::mutex g_players_m;
std::vector<std::weak_ptr<void>> g_players;
std::atomic<double> g_live_volume{1.0};
std::atomic<bool> g_live_volume_set{false};

}

void WinRtPlayer::adopt() noexcept
{
    try {
        const std::scoped_lock lk(g_players_m);
        std::erase_if(g_players, [](const std::weak_ptr<void>& w) { return w.expired(); });
        g_players.emplace_back(impl_);
    } catch (...) { // NOLINT(bugprone-empty-catch): the player then just misses live volume changes
    }
}

void WinRtPlayer::set_volume_all(double volume) noexcept
{
    const double v = std::clamp(volume, 0.0, 1.0);
    g_live_volume = v;
    g_live_volume_set = true;
    try {
        std::vector<std::shared_ptr<Impl>> live;
        {
            const std::scoped_lock lk(g_players_m);
            for (const auto& w : g_players) {
                if (auto impl = std::static_pointer_cast<Impl>(w.lock()))
                    live.push_back(std::move(impl));
            }
        }
        for (const auto& impl : live) {
            const std::scoped_lock pl(impl->m);
            if (impl->mp)
                impl->mp.Volume(v);
        }
    } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
    }
}

bool WinRtPlayer::play(double volume, OnEvent on_event, double rate) noexcept
{
    try {
        const std::scoped_lock lk(impl_->m);
        auto h = std::make_shared<Hook>();
        h->on_event = std::move(on_event);
        impl_->mp.Volume(g_live_volume_set ? g_live_volume.load() : std::clamp(volume, 0.0, 1.0));
        if (std::abs(rate - 1.0) > 1e-3)
            impl_->mp.PlaybackSession().PlaybackRate(std::clamp(rate, 0.5, 3.0));
        hook_player(impl_->mp, impl_->item, h);
        impl_->play_when_open(std::clamp(rate, 0.5, 3.0));
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
        impl_->cancel_pending(false);
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
            impl_->play_when_open(impl_->gate->rate);
    } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
    }
}

void WinRtPlayer::close() noexcept
{
    if (!impl_)
        return;
    const std::scoped_lock lk(impl_->m);
    try {
        impl_->cancel_pending(true);
        if (impl_->mp) {
            impl_->mp.Pause();
            impl_->mp.Close();
        }
    } catch (...) { // NOLINT(bugprone-empty-catch): no exception crosses a boundary
    }
    impl_->mp = nullptr;
}

}
