#include "doc/TxtDecode.h"
#include "doc/TxtToFb2.h"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    const std::span raw(reinterpret_cast<const std::byte*>(data), size);
    (void)eb::decode_text(raw);
    (void)eb::txt_to_fb2(raw, L"fuzz");
    return 0;
}
