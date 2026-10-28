extern "C" {
#include "eb_sfnt.h"
}

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

struct Mem {
    const uint8_t* data;
    size_t size;
};

int mem_read(void* user, uint64_t off, void* dst, size_t len)
{
    const auto* m = static_cast<const Mem*>(user);
    if (off > m->size || len > m->size - off) __builtin_trap();
    std::memcpy(dst, m->data + off, len);
    return 1;
}

}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    Mem m{data, size};
    const int faces = eb_sfnt_face_count(mem_read, &m, size);
    for (int i = 0; i < faces && i < EB_SFNT_MAX_FACES; ++i) {
        eb_sfnt_face face;
        eb_sfnt_probe(mem_read, &m, size, i, &face);
    }
    eb_sfnt_face face;
    eb_sfnt_probe(mem_read, &m, size, faces, &face);
    return 0;
}
