#pragma once
#include <cstdint>
#include <string_view>

namespace eb {

enum class Lang : std::uint8_t { En, Vi };

Lang detect_sentence(std::string_view utf8, Lang book_default) noexcept;

Lang book_language(std::string_view lang_tag, std::string_view sample_utf8) noexcept;

}
