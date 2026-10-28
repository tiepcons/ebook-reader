#include "tts/azure/AzurePlayer.h"

#include "tts/winrt/WinRtPlayer.h"

#include <optional>

namespace eb {

struct AzurePlayer::Impl {
    std::shared_ptr<AzureSink> sink;
    std::optional<WinRtApartment> apartment;
    WinRtPlayer cur;
    std::unique_ptr<AzureTaskThread> thread;

    void fail(uint32_t gen, uint32_t id) noexcept { sink->post(gen, id, TtsEvent::Error, kTtsPlayFailed); }

    void play(uint32_t gen, uint32_t id, const AzureAudio& audio, double volume)
    {
        if (!sink->current(gen))
            return;
        WinRtPlayer p;
        if (!p.open_bytes(*audio, L"audio/mpeg")) {
            fail(gen, id);
            return;
        }
        cur.close();
        cur = p;
        const bool ok = cur.play(volume, [s = sink, gen, id](PlayEvent ev, uint32_t) {
            switch (ev) {
            case PlayEvent::Started: s->post(gen, id, TtsEvent::Started, 0); break;
            case PlayEvent::Finished: s->post(gen, id, TtsEvent::Finished, 0); break;
            case PlayEvent::Failed: s->post(gen, id, TtsEvent::Error, kTtsPlayFailed); break;
            case PlayEvent::Word: break;
            }
        });
        if (!ok) {
            cur.close();
            fail(gen, id);
        }
    }
};

AzurePlayer::AzurePlayer(std::shared_ptr<AzureSink> sink) : impl_(std::make_unique<Impl>())
{
    impl_->sink = std::move(sink);
    Impl* im = impl_.get();
    impl_->thread = std::make_unique<AzureTaskThread>(
        [im] { im->apartment.emplace(); },
        [im] {
            im->cur.close();
            im->apartment.reset();
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
    impl_->thread->push([im = impl_.get()] { im->cur.pause(); });
}

void AzurePlayer::resume() noexcept
{
    impl_->thread->push([im = impl_.get()] { im->cur.resume(); });
}

void AzurePlayer::stop() noexcept
{
    impl_->thread->push([im = impl_.get()] { im->cur.close(); });
}

}
