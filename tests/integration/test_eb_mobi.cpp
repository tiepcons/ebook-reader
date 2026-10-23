#include "eb_test_util.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>

namespace {

constexpr uint32_t kNotSet = 0xFFFFFFFFu;

void put16(std::string& s, size_t at, uint32_t v)
{
    s[at] = static_cast<char>((v >> 8) & 0xFFu);
    s[at + 1] = static_cast<char>(v & 0xFFu);
}

void put32(std::string& s, size_t at, uint32_t v)
{
    for (size_t i = 0; i < 4; ++i) s[at + i] = static_cast<char>((v >> (24 - 8 * i)) & 0xFFu);
}

std::string be32(uint32_t v)
{
    std::string s(4, '\0');
    put32(s, 0, v);
    return s;
}

std::string exth_record(uint32_t tag, const std::string& data) { return be32(tag) + be32(static_cast<uint32_t>(data.size() + 8)) + data; }

std::string make_exth()
{
    const std::string records = exth_record(100, "Test Author") + exth_record(99, "Test Title") + exth_record(524, "vi") +
                                exth_record(201, be32(0));
    std::string e = "EXTH" + be32(static_cast<uint32_t>(12 + records.size())) + be32(4) + records;
    while (e.size() % 4 != 0) e.push_back('\0');
    return e;
}

std::string make_bmp(bool top_dark)
{
    const int w = 8;
    const int h = 8;
    std::string pixels;
    for (int row = 0; row < h; ++row) {
        const bool file_row_is_top = row >= h / 2;
        const char v = (file_row_is_top == top_dark) ? '\0' : '\xff';
        pixels.append(static_cast<size_t>(w) * 3, v);
    }
    std::string b = "BM";
    auto le32 = [&b](uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<char>((v >> (8 * i)) & 0xFFu));
    };
    le32(static_cast<uint32_t>(54 + pixels.size()));
    le32(0);
    le32(54);
    le32(40);
    le32(w);
    le32(h);
    b += std::string("\x01\x00\x18\x00", 4);
    le32(0);
    le32(static_cast<uint32_t>(pixels.size()));
    le32(2835);
    le32(2835);
    le32(0);
    le32(0);
    return b + pixels;
}

struct MobiSpec {
    uint32_t version = 6;
    uint32_t compression = 1;
    std::map<size_t, uint32_t> mobi32;  // offsets relative to the "MOBI" magic
    std::map<size_t, uint32_t> rec0_16; // 16-bit fields at offsets in record 0
    std::map<size_t, uint32_t> rec0_32; // 32-bit fields at offsets in record 0
    std::vector<std::string> extra;     // records after the cover image
    bool top_dark = true;
};

std::string make_record0(const MobiSpec& spec, size_t text_len)
{
    const std::string exth = make_exth();
    const std::string name = "Synthetic";
    std::string r(16 + 232, '\0');
    put16(r, 0, spec.compression);
    put32(r, 4, static_cast<uint32_t>(text_len));
    put16(r, 8, 1);
    put16(r, 10, 4096);
    std::memcpy(&r[16], "MOBI", 4);
    auto mobi = [&r](size_t off, uint32_t v) { put32(r, 16 + off, v); };
    mobi(4, 232);
    mobi(8, 2);
    mobi(12, 65001);
    mobi(20, 6);
    for (size_t off = 24; off <= 64; off += 4) mobi(off, kNotSet);
    mobi(68, static_cast<uint32_t>(248 + exth.size()));
    mobi(72, static_cast<uint32_t>(name.size()));
    mobi(76, 9);
    mobi(88, 6);
    mobi(92, 2);
    mobi(96, kNotSet);
    mobi(104, kNotSet);
    mobi(112, 0x40);
    mobi(176, kNotSet);
    mobi(228, kNotSet);
    mobi(20, spec.version);
    for (const auto& [off, v] : spec.mobi32) mobi(off, v);
    for (const auto& [off, v] : spec.rec0_16) put16(r, off, v);
    for (const auto& [off, v] : spec.rec0_32) put32(r, off, v);
    return r + exth + name;
}

std::string make_mobi(const MobiSpec& spec)
{
    const std::string text = "<html><head><title>Synthetic</title></head><body><p>Hello synthetic world.</p></body></html>";
    std::vector<std::string> recs = {make_record0(spec, text.size()), text, make_bmp(spec.top_dark)};
    for (const auto& e : spec.extra) recs.push_back(e);
    std::string f(78, '\0');
    std::memcpy(&f[0], "synthetic", 9);
    std::memcpy(&f[60], "BOOKMOBI", 8);
    put16(f, 76, static_cast<uint32_t>(recs.size()));
    size_t offset = 78 + 8 * recs.size();
    std::string list;
    std::string body;
    for (size_t i = 0; i < recs.size(); ++i) {
        list += be32(static_cast<uint32_t>(offset)) + be32(static_cast<uint32_t>(2 * i));
        offset += recs[i].size();
        body += recs[i];
    }
    return f + list + body;
}

eb_status open_blob(const std::string& blob, DocPtr& out)
{
    eb_doc* d = nullptr;
    const eb_status st = eb_open_memory(blob.data(), blob.size(), ".mobi", &d);
    out.reset(d);
    return st;
}

std::string read_file(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void expect_rejected(const MobiSpec& spec, const char* what)
{
    DocPtr d;
    const eb_status st = open_blob(make_mobi(spec), d);
    const std::string detail = std::string(what) + " -> " + eb_status_text(st) + ": " + eb_last_error();
    INFO(detail);
    CHECK(st == EB_E_FORMAT);
    CHECK(d == nullptr);
    CHECK(std::strstr(eb_last_error(), what) != nullptr);
}

std::string simple_record(const char* magic, size_t size)
{
    std::string r(size, '\0');
    std::memcpy(&r[0], magic, 4);
    return r;
}

} // namespace

TEST_CASE("eb_mobi reads Kindle books through libmobi with metadata and cover")
{
    const Lib lib;
    for (const char* sub : {"good/mobi", "good/azw3"}) {
        const std::string expected = std::string(sub) == "good/azw3" ? "AZW3" : "MOBI";
        const auto files = corpus(sub);
        REQUIRE_FALSE(files.empty());
        for (const auto& p : files) {
            INFO(p.string());
            const DocPtr d = open_doc(p);
            char fmt[16] = {};
            size_t needed = 0;
            REQUIRE(eb_meta(d.get(), "format", fmt, sizeof fmt, &needed) == EB_OK);
            CHECK(std::string(fmt) == expected);
            char lang[32] = {};
            CHECK(eb_meta(d.get(), "language", lang, sizeof lang, &needed) == EB_OK);
            CHECK(lang[0] != '\0');
            char title[256] = {};
            CHECK(eb_meta(d.get(), "title", title, sizeof title, &needed) == EB_OK);
            eb_bitmap* raw = nullptr;
            REQUIRE(eb_render_cover(d.get(), 200, &raw) == EB_OK);
            const BitmapPtr cover(raw);
            CHECK(std::max(cover->width, cover->height) == 200);
            CHECK(not_uniform(*cover));
            eb_toc_item* items = nullptr;
            int32_t n = 0;
            REQUIRE(eb_toc(d.get(), &items, &n) == EB_OK);
            CHECK(n > 0);
            eb_toc_free(items, n);
        }
    }
}

TEST_CASE("eb_mobi synthetic book opens and the embedded cover is upright")
{
    const Lib lib;
    for (const bool top_dark : {true, false}) {
        MobiSpec spec;
        spec.top_dark = top_dark;
        DocPtr d;
        REQUIRE(open_blob(make_mobi(spec), d) == EB_OK);
        char lang[16] = {};
        size_t needed = 0;
        REQUIRE(eb_meta(d.get(), "language", lang, sizeof lang, &needed) == EB_OK);
        CHECK(std::string(lang) == "vi");
        eb_bitmap* raw = nullptr;
        REQUIRE(eb_render_cover(d.get(), 64, &raw) == EB_OK);
        const BitmapPtr cover(raw);
        REQUIRE(cover->width == 64);
        REQUIRE(cover->height == 64);
        const auto lum = [&](int y) {
            const size_t offset = static_cast<size_t>(y) * static_cast<size_t>(cover->stride) + size_t{32} * 4;
            const uint8_t* px = cover->bgra + offset;
            return (px[0] + px[1] + px[2]) / 3;
        };
        CHECK((lum(2) < 64) == top_dark);
        CHECK((lum(61) < 64) == !top_dark);
        CHECK((lum(2) > 192) == !top_dark);
        CHECK((lum(61) > 192) == top_dark);
    }
}

TEST_CASE("eb_mobi structural pre-checks reject hostile books")
{
    const Lib lib;
    {
        MobiSpec s;
        s.rec0_16[10] = 0xFFFF;
        expect_rejected(s, "precheck: text record size");
    }
    {
        MobiSpec s;
        s.rec0_32[4] = 0xFFFFFFF0u;
        expect_rejected(s, "precheck: text length");
    }
    {
        MobiSpec s;
        s.version = 8;
        s.mobi32[176] = 3;
        s.mobi32[180] = 2;
        std::string fdst = simple_record("FDST", 28);
        put32(fdst, 4, 12);
        put32(fdst, 8, 2);
        put32(fdst, 12, 5);
        put32(fdst, 16, 100);
        put32(fdst, 20, 100);
        put32(fdst, 24, 200);
        s.extra = {fdst};
        expect_rejected(s, "precheck: fdst");
    }
    {
        MobiSpec s;
        s.extra = {simple_record("FONT", 32)};
        put32(s.extra[0], 4, 0x7FFFFFFFu);
        expect_rejected(s, "precheck: font");
    }
    {
        MobiSpec s;
        s.mobi32[24] = 1;
        expect_rejected(s, "precheck: dictionaries");
    }
    {
        MobiSpec s;
        s.mobi32[228] = 3;
        std::string indx = simple_record("INDX", 64);
        put32(indx, 4, 56);
        put32(indx, 24, 1);
        put32(indx, 36, 1000000);
        s.extra = {indx};
        expect_rejected(s, "precheck: index entry count");
    }
    {
        MobiSpec s;
        s.mobi32[228] = 3;
        std::string indx = simple_record("INDX", 80);
        put32(indx, 4, 56);
        put32(indx, 24, 1);
        std::memcpy(&indx[56], "TAGX", 4);
        put32(indx, 64, 99);
        s.extra = {indx};
        expect_rejected(s, "precheck: index tag table");
    }
    {
        // Meta record without TAGX and total=0; a later record carries the bad TAGX (libmobi re-reads it).
        MobiSpec s;
        s.mobi32[228] = 3;
        std::string meta = simple_record("INDX", 64);
        put32(meta, 4, 56);
        put32(meta, 24, 1);
        std::string later = simple_record("INDX", 80);
        put32(later, 4, 56);
        put32(later, 24, 1);
        put32(later, 36, 1);
        std::memcpy(&later[56], "TAGX", 4);
        put32(later, 64, 99);
        s.extra = {meta, later};
        expect_rejected(s, "precheck: index tag table");
    }
    {
        MobiSpec s;
        s.mobi32[228] = 3;
        std::string indx = simple_record("INDX", 80);
        put32(indx, 4, 56);
        put32(indx, 24, 1);
        std::memcpy(&indx[56], "TAGX", 4);
        put32(indx, 64, 1);
        s.extra = {indx};
        expect_rejected(s, "precheck: index without entries");
    }
    {
        MobiSpec s;
        s.compression = 17480;
        s.mobi32[96] = 3;
        s.mobi32[100] = 2;
        std::string huff = simple_record("HUFF", 24 + 1024 + 256);
        put32(huff, 4, 24);
        put32(huff, 8, 24);
        put32(huff, 12, 24 + 1024);
        s.extra = {huff};
        expect_rejected(s, "precheck: huff code length");
    }
}

TEST_CASE("eb_mobi damaged files fail cleanly or fall back to MuPDF")
{
    const Lib lib;
    const auto mobis = corpus("good/mobi");
    REQUIRE_FALSE(mobis.empty());
    const std::string data = read_file(mobis[0]);
    REQUIRE(data.size() > 4096);
    DocPtr d;

    const std::string cut = data.substr(0, 10000);
    const eb_status cutStatus = open_blob(cut, d);
    CHECK((cutStatus == EB_OK) == (d != nullptr));

    std::string table = data;
    for (size_t i = 78; i < 82; ++i) table[i] = '\xff';
    CHECK(open_blob(table, d) != EB_OK);
    CHECK(d == nullptr);

    std::string exth = data;
    const size_t at = exth.find("EXTH");
    REQUIRE(at != std::string::npos);
    put32(exth, at + 4, 0xFFFFFFF0u);
    const eb_status st = open_blob(exth, d);
    INFO(eb_status_text(st) << ": " << eb_last_error());
    CHECK((st == EB_OK) == (d != nullptr));
}

TEST_CASE("eb_mobi memory budget turns exhaustion into EB_E_NOMEM")
{
    const auto files = corpus("good/azw3");
    REQUIRE_FALSE(files.empty());
    const std::string data = read_file(files[0]);
    eb_limits lim{};
    lim.max_heap_bytes = 1000000;
    eb_status st = eb_init(&lim);
    if (st == EB_OK) {
        {
            eb_doc* raw = nullptr;
            st = eb_open_memory(data.data(), data.size(), ".azw3", &raw);
            const DocPtr d(raw);
            CHECK(d == nullptr);
        }
        eb_shutdown();
    }
    INFO(eb_status_text(st) << ": " << eb_last_error());
    CHECK(st == EB_E_NOMEM);
}
