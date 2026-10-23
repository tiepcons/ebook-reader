#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace eb {

struct Sentence {
    uint32_t begin;
    uint32_t end;
    bool operator==(const Sentence&) const = default;
};

constexpr size_t kMaxSentenceChars = 400;

std::vector<Sentence> split_sentences(std::string_view utf8) noexcept;

std::string sentence_text(std::string_view utf8, Sentence s) noexcept;

std::string_view strip_page_furniture(std::string_view utf8) noexcept;

}
