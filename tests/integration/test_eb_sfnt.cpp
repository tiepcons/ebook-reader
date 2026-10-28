#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include "eb_sfnt.h"
}

namespace {

using Bytes = std::vector<uint8_t>;

struct Mem {
    Bytes data;
    bool out_of_range = false;
};

int mem_read(void* user, uint64_t off, void* dst, size_t len)
{
    auto* m = static_cast<Mem*>(user);
    if (off > m->data.size() || len > m->data.size() - off) {
        m->out_of_range = true;
        return 0;
    }
    std::memcpy(dst, m->data.data() + off, len);
    return 1;
}

void put16(Bytes& b, unsigned v)
{
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v));
}

void put32(Bytes& b, uint32_t v)
{
    put16(b, v >> 16);
    put16(b, v & 0xFFFFu);
}

Bytes name_table(const std::u16string& family, unsigned name_id, unsigned string_offset_extra = 0)
{
    Bytes t;
    put16(t, 0);
    put16(t, 1);
    put16(t, 18);
    put16(t, 3);
    put16(t, 1);
    put16(t, 0x409);
    put16(t, name_id);
    put16(t, static_cast<unsigned>(family.size() * 2));
    put16(t, string_offset_extra);
    for (const char16_t c : family) put16(t, c);
    return t;
}

Bytes os2_table(unsigned fs_selection)
{
    Bytes t(64, 0);
    t[62] = static_cast<uint8_t>(fs_selection >> 8);
    t[63] = static_cast<uint8_t>(fs_selection);
    return t;
}

Bytes make_font(const std::vector<std::pair<std::string, Bytes>>& tables)
{
    Bytes f;
    put32(f, 0x00010000u);
    put16(f, static_cast<unsigned>(tables.size()));
    put16(f, 0);
    put16(f, 0);
    put16(f, 0);
    uint32_t off = 12 + 16 * static_cast<uint32_t>(tables.size());
    for (const auto& t : tables) {
        f.insert(f.end(), t.first.begin(), t.first.end());
        put32(f, 0);
        put32(f, off);
        put32(f, static_cast<uint32_t>(t.second.size()));
        off += static_cast<uint32_t>(t.second.size());
    }
    for (const auto& t : tables) f.insert(f.end(), t.second.begin(), t.second.end());
    return f;
}

bool probe(Mem& m, int index, eb_sfnt_face* out)
{
    return eb_sfnt_probe(mem_read, &m, m.data.size(), index, out) != 0;
}

Bytes read_file(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("eb_sfnt reads family and style from a minimal font")
{
    Mem m{make_font({{"name", name_table(u"Test Family", 1)}, {"OS/2", os2_table(0x21)}})};
    eb_sfnt_face f{};
    REQUIRE(probe(m, 0, &f));
    CHECK(std::string(f.family) == "Test Family");
    CHECK(f.bold == 1);
    CHECK(f.italic == 1);
    CHECK(eb_sfnt_face_count(mem_read, &m, m.data.size()) == 1);
    CHECK_FALSE(m.out_of_range);
    CHECK_FALSE(probe(m, 1, &f));
}

TEST_CASE("eb_sfnt falls back to head macStyle and prefers the typographic family")
{
    Bytes head(54, 0);
    head[45] = 0x02;
    Mem m{make_font({{"head", head}, {"name", name_table(u"Typographic", 16)}})};
    eb_sfnt_face f{};
    REQUIRE(probe(m, 0, &f));
    CHECK(std::string(f.family) == "Typographic");
    CHECK(f.bold == 0);
    CHECK(f.italic == 1);
}

TEST_CASE("eb_sfnt rejects truncated directories and out-of-range tables")
{
    eb_sfnt_face f{};
    Mem truncated{make_font({{"name", name_table(u"Cut", 1)}, {"OS/2", os2_table(0)}})};
    truncated.data.resize(12 + 16);
    CHECK_FALSE(probe(truncated, 0, &f));
    CHECK_FALSE(truncated.out_of_range);

    Mem tiny{Bytes{0, 1, 0, 0}};
    CHECK_FALSE(probe(tiny, 0, &f));
    CHECK(eb_sfnt_face_count(mem_read, &tiny, tiny.data.size()) == 0);

    Mem outside{make_font({{"name", name_table(u"Outside", 1, 0xFFF0)}})};
    CHECK_FALSE(probe(outside, 0, &f));
    CHECK_FALSE(outside.out_of_range);

    Bytes cut = make_font({{"name", name_table(u"Far", 1)}});
    cut.resize(cut.size() - 8);
    Mem past{cut};
    CHECK_FALSE(probe(past, 0, &f));
    CHECK_FALSE(past.out_of_range);
}

TEST_CASE("eb_sfnt caps a hostile collection face count")
{
    const Bytes ttc = {'t', 't', 'c', 'f', 0, 1, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF};
    Mem m{ttc};
    CHECK(eb_sfnt_face_count(mem_read, &m, m.data.size()) == EB_SFNT_MAX_FACES);
    eb_sfnt_face f{};
    for (int i = 0; i < EB_SFNT_MAX_FACES + 2; ++i) CHECK_FALSE(probe(m, i, &f));
    CHECK_FALSE(m.out_of_range);
    Mem zero{Bytes{'t', 't', 'c', 'f', 0, 1, 0, 0, 0, 0, 0, 0}};
    CHECK(eb_sfnt_face_count(mem_read, &zero, zero.data.size()) == 0);
}

TEST_CASE("eb_sfnt reads the bundled fonts")
{
    const std::filesystem::path dir = EB_FONTS_DIR;
    struct Case {
        const char* file;
        const char* family;
        int bold, italic;
    };
    const Case cases[] = {{"BeVietnamPro/BeVietnamPro-Regular.ttf", "Be Vietnam Pro", 0, 0},
                          {"BeVietnamPro/BeVietnamPro-BoldItalic.ttf", "Be Vietnam Pro", 1, 1},
                          {"Literata/Literata[opsz,wght].ttf", "Literata", 0, 0},
                          {"Literata/Literata-Italic[opsz,wght].ttf", "Literata", 0, 1}};
    for (const auto& c : cases) {
        INFO(c.file);
        Mem m{read_file(dir / c.file)};
        REQUIRE_FALSE(m.data.empty());
        eb_sfnt_face f{};
        REQUIRE(probe(m, 0, &f));
        CHECK(std::string(f.family) == c.family);
        CHECK(f.bold == c.bold);
        CHECK(f.italic == c.italic);
        CHECK_FALSE(m.out_of_range);
    }
}

TEST_CASE("eb_sfnt never reads past a file shorter than a table directory")
{
    for (const size_t len : {size_t{0}, size_t{1}, size_t{3}, size_t{4}, size_t{11}}) {
        Mem m{Bytes(len, 0)};
        eb_sfnt_face f{};
        CHECK_FALSE(probe(m, 0, &f));
        CHECK_EQ(eb_sfnt_face_count(mem_read, &m, len), 0);
        CHECK_FALSE(m.out_of_range);
    }
    Mem m{read_file(std::filesystem::path(EB_CORPUS_DIR) / "bad" / "empty.ttf")};
    eb_sfnt_face f{};
    CHECK_FALSE(probe(m, 0, &f));
    CHECK_FALSE(m.out_of_range);
}
