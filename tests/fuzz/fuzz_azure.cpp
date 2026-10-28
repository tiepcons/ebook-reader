#include "tts/azure/AzureProto.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    (void)eb::parse_voices(text, L"vi-VN-HoaiMyNeural");
    (void)eb::xml_escape(text);
    const auto cut = text.find('\n');
    const auto head = text.substr(0, cut);
    const auto rest = cut == std::string_view::npos ? std::string_view{} : text.substr(cut + 1);
    (void)eb::valid_azure_region(head);
    (void)eb::valid_azure_voice(head);
    (void)eb::build_ssml(rest, head, 0);
    double rate = 1.0;
    if (size >= sizeof rate) std::memcpy(&rate, data, sizeof rate);
    (void)eb::ssml_voice_body(rest, eb::rate_percent(rate));
    return 0;
}
