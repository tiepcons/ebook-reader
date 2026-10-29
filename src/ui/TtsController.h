#pragma once
#include "Tray.h"
#include "TtsPrefs.h"
#include "tts/Segmenter.h"
#include "tts/TtsEngine.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace eb {

inline constexpr int kMaxEmptyPages = 5;

// What the controller needs from the reader. Every call comes from the UI thread.
class TtsHost {
public:
    virtual ~TtsHost() = default;
    virtual void request_text(int32_t page) = 0; // the answer comes back through TtsController::on_text
    [[nodiscard]] virtual int32_t shown_page() const noexcept = 0;
    [[nodiscard]] virtual int32_t page_count() const noexcept = 0;
    // Saved sentence offset of the shown page, -1 when there is none.
    [[nodiscard]] virtual int64_t resume_offset() const noexcept = 0;
    virtual void turn_to(int32_t page) = 0;
    virtual void highlight(int32_t page, uint32_t begin, uint32_t end) = 0;
    virtual void clear_highlight() = 0;
    virtual void save_position(int32_t page, uint32_t offset) = 0;
    virtual void state_changed(TtsState s) = 0;
    virtual void notice(const wchar_t* text) = 0;
};

// Azure failures that the offline Windows voices can cover.
[[nodiscard]] bool azure_should_fall_back(TtsKind active, uint32_t error) noexcept;
// True when the voice locale starts with the language code.
[[nodiscard]] bool voice_has_lang(const Voice& v, Lang lang) noexcept;
// The wanted voice if the engine lists it (or lists nothing yet), else the first voice of the language, else the first one.
[[nodiscard]] std::wstring pick_voice(const std::vector<Voice>& list, const std::wstring& wanted, Lang lang);

// Reads a book aloud sentence by sentence: asks the host for the text of each page, hands every sentence to the
// active engine, moves the highlight, turns the page at its end and records the position.
class TtsController {
public:
    using Engines = std::array<TtsEngine*, kTtsKinds>;

    TtsController(TtsHost& host, Engines engines) noexcept : host_(host), engines_(engines) {}
    TtsController(const TtsController&) = delete;
    TtsController& operator=(const TtsController&) = delete;

    void set_prefs(const TtsPrefs& p);
    void set_book_lang(Lang l) noexcept { book_lang_ = l; }
    [[nodiscard]] const TtsPrefs& prefs() const noexcept { return prefs_; }
    [[nodiscard]] TtsState state() const noexcept { return state_; }
    [[nodiscard]] bool available(TtsKind k) const noexcept
    {
        return engines_[kind_index(k)] != nullptr && engines_[kind_index(k)]->ready();
    }
    [[nodiscard]] TtsKind active() const noexcept { return active_; }
    std::vector<Voice> voices(TtsKind k);

    void start(int32_t page, int64_t char_offset);
    void toggle();
    void stop();
    void step(int delta);
    void set_rate(double rate);
    void on_text(int32_t page, std::string utf8);
    void on_event(uint32_t utterance, LPARAM lp);
    // The view changed layout: the page numbers and texts are stale, so stop now and restart once the new page is shown.
    void relayout();
    void relaid();
    // The reader reports a page turn; one the controller did not make restarts reading at the shown page.
    void page_shown();

    void preview(TtsKind k, const std::wstring& voice, Lang lang, double rate);
    void end_preview();

private:
    struct Page {
        int32_t number = -1;
        std::string text;
        std::vector<Sentence> sentences;
    };

    void enter_page(Page p, int64_t offset);
    void go_next_page();
    bool take_next_page(Page& out);
    bool speak_sentence();
    void speak_current();
    void advance();
    void restart_at_shown();
    void prepare_sentence(const Page& p, std::size_t i, std::size_t ahead);
    void prefetch();
    void on_error(uint32_t code);
    void fall_back();
    void finish(const wchar_t* why);
    void set_state(TtsState s);
    [[nodiscard]] std::wstring voice_for(const std::string& sentence);
    [[nodiscard]] TtsEngine* engine() const noexcept { return engines_[kind_index(active_)]; }

    TtsHost& host_;
    Engines engines_;
    TtsPrefs prefs_;
    Lang book_lang_ = Lang::En;
    TtsState state_ = TtsState::Idle;
    TtsKind active_ = TtsKind::WinRt;
    std::array<std::vector<Voice>, kTtsKinds> voices_;
    Page page_, next_;
    std::size_t idx_ = 0;
    int32_t wait_page_ = -1, next_requested_ = -1, synced_ = -1;
    int64_t start_offset_ = -1;
    uint32_t counter_ = 0, current_ = 0;
    int empty_run_ = 0;
    bool fell_back_ = false, pending_advance_ = false, restart_ = false;
    std::optional<TtsKind> previewing_;
};

}
