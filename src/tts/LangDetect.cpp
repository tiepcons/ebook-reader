#include "LangDetect.h"

#include "Segmenter.h"
#include "Utf8.h"

#include <algorithm>
#include <array>
#include <string>

namespace eb {
namespace {

constexpr size_t kMaxSampleBytes = 16384;
constexpr size_t kMaxWords = 4096;

bool is_vi_marker(char32_t c) noexcept
{
    switch (c) {
    case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC8: case 0xC9: case 0xCA: case 0xCC: case 0xCD:
    case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD9: case 0xDA: case 0xDD:
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE8: case 0xE9: case 0xEA: case 0xEC: case 0xED:
    case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF9: case 0xFA: case 0xFD:
    case 0x102: case 0x103: case 0x110: case 0x111: case 0x128: case 0x129: case 0x168: case 0x169:
    case 0x1A0: case 0x1A1: case 0x1AF: case 0x1B0:
    case 0x300: case 0x301: case 0x303: case 0x309: case 0x31B: case 0x323:
        return true;
    default:
        return c >= 0x1EA0 && c <= 0x1EF9;
    }
}

bool is_english_stopword(std::string_view w) noexcept
{
    static constexpr std::array<std::string_view, 44> kWords = {"the", "and", "of", "in", "is", "are", "was", "were",
        "be", "been", "it", "that", "this", "with", "for", "on", "as", "at", "by", "from", "or", "not", "but", "have",
        "has", "had", "he", "she", "they", "we", "you", "i", "his", "her", "their", "its", "will", "would", "could",
        "which", "who", "what", "when", "there"};
    return std::ranges::find(kWords, w) != kWords.end();
}

struct Stats {
    size_t words = 0;
    size_t marked = 0;
    size_t english = 0;
};

void close_word(Stats& st, std::string& ascii, bool marked) noexcept
{
    ++st.words;
    st.marked += marked ? 1 : 0;
    st.english += (!marked && is_english_stopword(ascii)) ? 1 : 0;
    ascii.clear();
}

Stats scan_words(std::string_view s)
{
    Stats st;
    std::string ascii;
    bool in_word = false;
    bool marked = false;
    size_t i = 0;
    while (i < s.size() && st.words < kMaxWords) {
        const u8::Cp c = u8::decode(s, i);
        i += c.len;
        if (u8::is_alpha(c.cp)) {
            in_word = true;
            marked = marked || is_vi_marker(c.cp);
            if (c.cp < 0x80 && ascii.size() < 16)
                ascii.push_back(static_cast<char>(c.cp >= 'A' && c.cp <= 'Z' ? c.cp - 'A' + 'a' : c.cp));
        } else if (in_word) {
            close_word(st, ascii, marked);
            in_word = marked = false;
        }
    }
    if (in_word)
        close_word(st, ascii, marked);
    return st;
}

Lang classify(const Stats& st, Lang book_default) noexcept
{
    if (st.words == 0)
        return book_default;
    if (st.marked == 0)
        return (st.english > 0 || st.words >= 3) ? Lang::En : book_default;
    if (st.english >= 2 && st.english > st.marked)
        return Lang::En;
    if (st.marked == 1 && st.words >= 3)
        return st.english > 0 ? Lang::En : book_default;
    return st.marked * 100 >= st.words * 15 ? Lang::Vi : book_default;
}

bool tag_is(std::string_view tag, std::string_view two, std::string_view three) noexcept
{
    if (tag.size() < 2 || tag.substr(0, 2) != two)
        return false;
    return tag.size() == 2 || tag[2] == '-' || tag[2] == '_' || tag.substr(0, 3) == three;
}

}

Lang detect_sentence(std::string_view utf8, Lang book_default) noexcept
{
    try {
        return classify(scan_words(utf8), book_default);
    } catch (...) {
        return book_default;
    }
}

Lang book_language(std::string_view lang_tag, std::string_view sample_utf8) noexcept
{
    try {
        std::string tag(lang_tag);
        for (char& c : tag)
            c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        if (tag_is(tag, "vi", "vie"))
            return Lang::Vi;
        if (tag_is(tag, "en", "eng"))
            return Lang::En;
        const std::string_view sample = sample_utf8.substr(0, kMaxSampleBytes);
        size_t vi = 0;
        size_t en = 0;
        for (const Sentence& s : split_sentences(sample)) {
            const std::string_view part = sample.substr(s.begin, s.end - s.begin);
            (detect_sentence(part, Lang::En) == Lang::Vi ? vi : en)++;
        }
        return vi > en ? Lang::Vi : Lang::En;
    } catch (...) {
        return Lang::En;
    }
}

}
