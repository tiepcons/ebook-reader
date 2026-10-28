#include "tts/LangDetect.h"
#include "tts/Segmenter.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    for (const eb::Sentence s : eb::split_sentences(text)) {
        const auto sentence = eb::sentence_text(text, s);
        (void)eb::detect_sentence(sentence, eb::Lang::En);
    }
    (void)eb::strip_page_furniture(text);
    const auto tag = text.substr(0, text.find('\n'));
    (void)eb::book_language(tag, text);
    return 0;
}
