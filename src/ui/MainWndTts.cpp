#include "MainWnd.h"

#include "Kv.h"
#include "base/Log.h"
#include "base/SecretStore.h"
#include "base/Text.h"
#include "resource.h"
#include "tts/azure/AzureMessages.h"

#include <cmath>
#include <utility>

namespace eb {

void MainWnd::load_tts_prefs_once()
{
    if (tts_prefs_loaded_)
        return;
    tts_prefs_ = load_tts_prefs(db_);
    tts_prefs_loaded_ = true;
    tts_bar_.set_speed(tts_prefs_.rate);
}

bool MainWnd::ensure_tts()
{
    if (tts_)
        return true;
    load_tts_prefs_once();
    try {
        tts_engines_ = std::make_unique<TtsEngines>(make_tts_engines(paths_, db_, hwnd_));
        tts_ = std::make_unique<TtsController>(static_cast<TtsHost&>(*this), tts_engines_->pointers());
    } catch (...) {
        log::error("tts: cannot start the controller");
        tts_.reset();
        tts_engines_.reset();
        return false;
    }
    tts_->set_prefs(tts_prefs_);
    refresh_azure_quota();
    return true;
}

void MainWnd::reset_tts()
{
    if (tts_)
        tts_->stop();
    tts_.reset();
    tts_engines_.reset();
}

std::string MainWnd::book_lang_tag() const
{
    auto st = db_.prepare("SELECT lang FROM books WHERE id=?1");
    if (!st || !st->bind_text(1, reading_id_))
        return {};
    const auto step = st->step();
    return step && *step == Step::Row && !st->is_null(0) ? std::string(st->column_text(0)) : std::string{};
}

void MainWnd::tts_toggle()
{
    if (!in_reader_ || !ensure_tts())
        return;
    tts_lang_pending_ = tts_->state() == TtsState::Idle;
    tts_->toggle();
}

void MainWnd::tts_stop()
{
    if (tts_)
        tts_->stop();
}

void MainWnd::on_tts_text(int page)
{
    if (!tts_)
        return;
    const auto text = reader_.text_of(page);
    const std::string_view utf8 = text ? std::string_view(text->utf8) : std::string_view{};
    if (std::exchange(tts_lang_pending_, false))
        tts_->set_book_lang(book_language(book_lang_tag(), utf8));
    tts_->on_text(page, std::string(utf8));
}

bool MainWnd::tts_button(int id)
{
    if (id == kIdTtsPlay) {
        tts_toggle();
    } else if (id == kIdTtsStop) {
        tts_stop();
    } else if (id == kIdTtsPrev || id == kIdTtsNext) {
        if (tts_)
            tts_->step(id == kIdTtsNext ? 1 : -1);
    } else if (id == kIdTtsSettings) {
        open_tts_settings();
    } else {
        return false;
    }
    return true;
}

bool MainWnd::on_tts_command(WPARAM wp)
{
    const int id = LOWORD(wp), code = HIWORD(wp);
    if (id == kIdTtsSpeed && code == CBN_SELCHANGE) {
        tts_prefs_.rate = tts_bar_.speed();
        if (tts_)
            tts_->set_rate(tts_prefs_.rate);
        if (!save_tts_prefs(db_, tts_prefs_))
            log::error("tts: cannot save the speed");
    } else if (code != BN_CLICKED || !tts_button(id)) {
        return false;
    }
    SetFocus(reader_.hwnd());
    return true;
}

bool MainWnd::on_tts_msg(UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_APP_TTS_EVENT) {
        if (tts_) {
            tts_->on_event(static_cast<uint32_t>(wp), lp);
            smoke_tts_event(static_cast<uint32_t>(wp), lp);
            if (wp != 0 && tts_event(lp) == TtsEvent::Started)
                refresh_azure_quota();
        }
        return true;
    }
    if (msg == WM_APP_AZURE_WARN) {
        on_azure_warn(static_cast<Warn>(wp));
        return true;
    }
    if (msg != WM_APP_TTS_CMD)
        return false;
    if (in_settings_)
        return true;
    const auto cmd = static_cast<TtsCmd>(wp);
    if (cmd == TtsCmd::PlayPause)
        tts_toggle();
    else if (cmd == TtsCmd::Next && tts_)
        tts_->step(1);
    else if (cmd == TtsCmd::Stop)
        tts_stop();
    return true;
}

void MainWnd::open_tts_settings()
{
    if (!in_reader_ || in_settings_)
        return;
    if (!ensure_tts()) {
        show_notice(L"Không khởi động được bộ máy đọc.");
        return;
    }
    const bool had_key = secret_get(kAzureSecretTarget).has_value();
    azure_dirty_ = false;
    in_settings_ = true;
    const auto accepted = show_tts_settings(inst_, hwnd_, tts_prefs_, *this);
    in_settings_ = false;
    if (accepted) {
        tts_prefs_ = *accepted;
        tts_bar_.set_speed(tts_prefs_.rate);
        if (!save_tts_prefs(db_, tts_prefs_))
            log::error("tts: cannot save the voice settings");
    }
    if (azure_dirty_ || had_key != secret_get(kAzureSecretTarget).has_value())
        rebuild_azure_engine();
    else if (accepted && tts_)
        tts_->set_prefs(tts_prefs_);
    refresh_azure_quota();
    SetFocus(reader_.hwnd());
}

void MainWnd::request_text(int32_t page)
{
    reader_.request_text(page);
}

int32_t MainWnd::shown_page() const noexcept
{
    return reader_.page();
}

int32_t MainWnd::page_count() const noexcept
{
    return reader_.page_count();
}

int64_t MainWnd::resume_offset() const noexcept
{
    return reader_.resume_tts_char();
}

void MainWnd::turn_to(int32_t page)
{
    reader_.turn_to(page);
}

void MainWnd::highlight(int32_t page, uint32_t begin, uint32_t end)
{
    const std::size_t rects = reader_.set_highlight(page, begin, end);
    if (smoke_)
        log::info("smoke: tts highlight {} rects on page {}", rects, page);
}

void MainWnd::clear_highlight()
{
    reader_.clear_highlight();
}

void MainWnd::save_position(int32_t page, uint32_t offset)
{
    reader_.save_tts_position(page, offset);
}

void MainWnd::state_changed(TtsState s)
{
    set_tts_state(s);
    tts_bar_.set_state(s);
    refresh_azure_quota();
    if (s == TtsState::Idle)
        tts_bar_.set_info(L"");
    else
        tts_bar_.set_info(s == TtsState::Paused ? L"Đang tạm dừng" : L"Đang đọc…");
}

void MainWnd::notice(const wchar_t* text)
{
    log::info("tts: {}", wide_to_utf8(text).value_or(std::string{}));
    tts_bar_.set_info(text);
    SendMessageW(status_, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(text));
}

std::vector<TtsKind> MainWnd::available()
{
    std::vector<TtsKind> out;
    for (const TtsKind k : {TtsKind::WinRt, TtsKind::Piper, TtsKind::Azure}) {
        if (tts_ && tts_->available(k))
            out.push_back(k);
    }
    return out;
}

std::vector<Voice> MainWnd::voices(TtsKind k)
{
    return tts_ ? tts_->voices(k) : std::vector<Voice>{};
}

void MainWnd::preview(TtsKind k, const std::wstring& voice, Lang lang, double rate)
{
    if (tts_)
        tts_->preview(k, voice, lang, rate);
}

void MainWnd::end_preview()
{
    if (tts_)
        tts_->end_preview();
}

void MainWnd::smoke_tts_event(uint32_t id, LPARAM lp)
{
    if (!smoke_tts_run_ || id == 0)
        return;
    const TtsEvent ev = tts_event(lp);
    if (ev != TtsEvent::Started && ev != TtsEvent::Error)
        return;
    smoke_tts_run_ = false;
    if (ev == TtsEvent::Started)
        log::info("smoke: tts started");
    else
        log::error("smoke: tts error {}", tts_value(lp));
    tts_stop();
    PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

}
