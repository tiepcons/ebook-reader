#include "TtsController.h"

#include <algorithm>
#include <cwctype>
#include <utility>

namespace eb {
namespace {

constexpr std::string_view kSampleEn = "Hello, this is a sample of the reading voice.";
constexpr std::string_view kSampleVi = "Xin chào, đây là giọng đọc thử của sách điện tử.";

std::string one_line(std::string s)
{
    std::ranges::replace(s, '\n', ' ');
    return s;
}

const wchar_t* error_text(uint32_t code) noexcept
{
    switch (code) {
    case kTtsBadVoice:
        return L"Không tìm thấy giọng đọc đã chọn. Hãy chọn giọng khác trong phần cài đặt đọc.";
    case kTtsPlayFailed:
        return L"Không phát được âm thanh.";
    case kTtsTextTooLarge:
        return L"Câu quá dài để đọc.";
    default:
        return L"Không đọc được câu này.";
    }
}

}

bool azure_should_fall_back(TtsKind active, uint32_t error) noexcept
{
    return active == TtsKind::Azure && (error == kTtsNetwork || error == kTtsQuota || error == kTtsAuth);
}

bool voice_has_lang(const Voice& v, Lang lang) noexcept
{
    const wchar_t* want = lang == Lang::Vi ? L"vi" : L"en";
    return v.lang.size() >= 2 && std::towlower(v.lang[0]) == want[0] && std::towlower(v.lang[1]) == want[1];
}

std::wstring pick_voice(const std::vector<Voice>& list, const std::wstring& wanted, Lang lang)
{
    if (!wanted.empty() && (list.empty() || std::ranges::any_of(list, [&](const Voice& v) { return v.id == wanted; })))
        return wanted;
    if (const auto it = std::ranges::find_if(list, [&](const Voice& v) { return voice_has_lang(v, lang); }); it != list.end())
        return it->id;
    return list.empty() ? std::wstring{} : list.front().id;
}

void TtsController::set_prefs(const TtsPrefs& p)
{
    if (state_ != TtsState::Idle && p.engine != prefs_.engine)
        stop();
    prefs_ = p;
    prefs_.rate = clamp_tts_rate(p.rate);
}

std::vector<Voice> TtsController::voices(TtsKind k)
{
    auto& cache = voices_[kind_index(k)];
    if (cache.empty() && engines_[kind_index(k)] != nullptr)
        cache = engines_[kind_index(k)]->voices();
    return cache;
}

void TtsController::set_state(TtsState s)
{
    if (s == state_)
        return;
    state_ = s;
    host_.state_changed(s);
}

void TtsController::start(int32_t page, int64_t char_offset)
{
    end_preview();
    stop();
    active_ = available(prefs_.engine) ? prefs_.engine : TtsKind::WinRt;
    if (engine() == nullptr) {
        host_.notice(L"Chưa có giọng đọc nào dùng được trên máy này.");
        return;
    }
    start_offset_ = char_offset;
    synced_ = host_.shown_page();
    empty_run_ = 0;
    fell_back_ = false;
    wait_page_ = next_requested_ = page;
    set_state(TtsState::Playing);
    host_.request_text(page);
}

void TtsController::toggle()
{
    if (state_ == TtsState::Idle) {
        start(host_.shown_page(), host_.resume_offset());
    } else if (state_ == TtsState::Paused) {
        engine()->resume();
        set_state(TtsState::Playing);
        if (std::exchange(pending_advance_, false))
            advance();
    } else if (wait_page_ >= 0) {
        stop();
    } else {
        engine()->pause();
        set_state(TtsState::Paused);
    }
}

void TtsController::stop()
{
    restart_ = pending_advance_ = false;
    if (state_ == TtsState::Idle)
        return;
    if (engine() != nullptr)
        engine()->stop();
    page_ = next_ = Page{};
    wait_page_ = next_requested_ = -1;
    current_ = 0;
    idx_ = 0;
    host_.clear_highlight();
    set_state(TtsState::Idle);
}

void TtsController::finish(const wchar_t* why)
{
    stop();
    if (why != nullptr)
        host_.notice(why);
}

void TtsController::on_text(int32_t page, std::string utf8)
{
    if (state_ == TtsState::Idle)
        return;
    Page p;
    p.number = page;
    const std::string_view body = strip_page_furniture(utf8);
    p.sentences = split_sentences(body);
    const auto base = static_cast<uint32_t>(body.data() - utf8.data());
    for (Sentence& s : p.sentences) {
        s.begin += base;
        s.end += base;
    }
    p.text = std::move(utf8);
    if (page == wait_page_) {
        wait_page_ = -1;
        enter_page(std::move(p), std::exchange(start_offset_, -1));
    } else if (page_.number >= 0 && page == page_.number + 1) {
        next_ = std::move(p);
        if (idx_ + 1 >= page_.sentences.size() && !next_.sentences.empty())
            prepare_sentence(next_, 0);
    }
}

void TtsController::enter_page(Page p, int64_t offset)
{
    for (;;) {
        if (host_.shown_page() != synced_) {
            restart_at_shown();
            return;
        }
        page_ = std::move(p);
        next_ = Page{};
        next_requested_ = -1;
        host_.turn_to(page_.number);
        synced_ = page_.number;
        if (!page_.sentences.empty())
            break;
        if (++empty_run_ > kMaxEmptyPages) {
            finish(L"Không tìm thấy văn bản để đọc trong các trang này.");
            return;
        }
        offset = -1;
        if (!take_next_page(p))
            return;
    }
    empty_run_ = 0;
    idx_ = 0;
    if (offset > 0) {
        const auto it = std::ranges::find_if(page_.sentences, [&](const Sentence& s) { return s.end > offset; });
        idx_ = it != page_.sentences.end() ? static_cast<std::size_t>(it - page_.sentences.begin()) : 0;
    }
    speak_current();
}

void TtsController::go_next_page()
{
    if (Page p; take_next_page(p))
        enter_page(std::move(p), -1);
}

bool TtsController::take_next_page(Page& out)
{
    const int32_t next = page_.number + 1;
    if (next >= host_.page_count()) {
        finish(L"Đã đọc hết sách.");
        return false;
    }
    if (next_.number == next) {
        out = std::exchange(next_, Page{});
        return true;
    }
    wait_page_ = next;
    if (next_requested_ != next) {
        next_requested_ = next;
        host_.request_text(next);
    }
    return false;
}

std::wstring TtsController::voice_for(const std::string& sentence)
{
    const Lang lang = detect_sentence(sentence, book_lang_);
    const TtsKind k = active_;
    if (voices_[kind_index(k)].empty())
        voices_[kind_index(k)] = engine()->voices();
    return pick_voice(voices_[kind_index(k)], prefs_.voice[kind_index(k)][static_cast<std::size_t>(lang)], lang);
}

bool TtsController::speak_sentence()
{
    const Sentence s = page_.sentences[idx_];
    const std::string text = one_line(sentence_text(page_.text, s));
    host_.highlight(page_.number, s.begin, s.end);
    host_.save_position(page_.number, s.begin);
    current_ = ++counter_;
    set_state(TtsState::Playing);
    return engine()->speak(current_, text, voice_for(text), prefs_.rate, 1.0);
}

void TtsController::speak_current()
{
    if (speak_sentence())
        prefetch();
    else
        on_error(kTtsSynthFailed);
}

void TtsController::prepare_sentence(const Page& p, std::size_t i)
{
    const std::string text = one_line(sentence_text(p.text, p.sentences[i]));
    engine()->prepare(counter_ + 1, text, voice_for(text), prefs_.rate);
}

void TtsController::prefetch()
{
    if (idx_ + 1 < page_.sentences.size()) {
        prepare_sentence(page_, idx_ + 1);
    } else if (next_.number == page_.number + 1 && !next_.sentences.empty()) {
        prepare_sentence(next_, 0);
    } else if (next_requested_ != page_.number + 1 && page_.number + 1 < host_.page_count()) {
        next_requested_ = page_.number + 1;
        host_.request_text(next_requested_);
    }
}

void TtsController::on_event(uint32_t utterance, LPARAM lp)
{
    if (state_ == TtsState::Idle || utterance == 0 || utterance != current_)
        return;
    const TtsEvent ev = tts_event(lp);
    if (ev == TtsEvent::Error) {
        on_error(tts_value(lp));
    } else if (ev == TtsEvent::Finished) {
        if (state_ == TtsState::Paused)
            pending_advance_ = true;
        else
            advance();
    }
}

void TtsController::advance()
{
    if (++idx_ < page_.sentences.size())
        speak_current();
    else
        go_next_page();
}

void TtsController::restart_at_shown()
{
    start(host_.shown_page(), -1);
}

void TtsController::relayout()
{
    if (state_ == TtsState::Idle)
        return;
    stop();
    restart_ = true;
}

void TtsController::relaid()
{
    if (std::exchange(restart_, false))
        start(host_.shown_page(), host_.resume_offset());
}

void TtsController::page_shown()
{
    if (state_ != TtsState::Idle && host_.shown_page() != synced_)
        restart_at_shown();
}

void TtsController::on_error(uint32_t code)
{
    if (azure_should_fall_back(active_, code) && !fell_back_ && engines_[kind_index(TtsKind::WinRt)] != nullptr) {
        fall_back();
        return;
    }
    finish(error_text(code));
}

void TtsController::fall_back()
{
    engine()->stop();
    active_ = TtsKind::WinRt;
    fell_back_ = true;
    host_.notice(L"Azure không dùng được lúc này, đã chuyển sang giọng Windows.");
    if (speak_sentence())
        prefetch();
    else
        finish(error_text(kTtsSynthFailed));
}

void TtsController::step(int delta)
{
    if (state_ == TtsState::Idle || wait_page_ >= 0 || page_.sentences.empty() || delta == 0)
        return;
    engine()->stop();
    pending_advance_ = false;
    if (delta > 0) {
        if (++idx_ >= page_.sentences.size()) {
            set_state(TtsState::Playing);
            go_next_page();
            return;
        }
    } else if (idx_ > 0) {
        --idx_;
    }
    speak_current();
}

void TtsController::set_rate(double rate)
{
    prefs_.rate = clamp_tts_rate(rate);
    if (state_ == TtsState::Playing && wait_page_ < 0 && !page_.sentences.empty()) {
        engine()->stop();
        speak_current();
    }
}

void TtsController::preview(TtsKind k, const std::wstring& voice, Lang lang, double rate)
{
    TtsEngine* e = engines_[kind_index(k)];
    if (state_ != TtsState::Idle || e == nullptr)
        return;
    end_preview();
    previewing_ = k;
    e->speak(++counter_, lang == Lang::Vi ? kSampleVi : kSampleEn, voice, clamp_tts_rate(rate), 1.0);
}

void TtsController::end_preview()
{
    if (previewing_ && engines_[kind_index(*previewing_)] != nullptr)
        engines_[kind_index(*previewing_)]->stop();
    previewing_.reset();
}

}
