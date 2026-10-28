#include "tts/piper/PiperTts.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    (void)eb::piper::json_escape(text);
    (void)eb::piper::request_line(text.substr(0, size / 2), text.substr(size / 2));
    (void)eb::piper::lang_from_model_json(text);
    std::wstring wide(size / sizeof(wchar_t), L'\0');
    std::memcpy(wide.data(), data, wide.size() * sizeof(wchar_t)); // no type-punned read
    (void)eb::piper::quote_arg(wide);
    return 0;
}
