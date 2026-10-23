#include "eb_test_util.h"

#include <cstring>
#include <string>
#include <utility>

namespace {

uint32_t crc32_of(const std::string& s)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (const unsigned char c : s) {
        crc ^= c;
        for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void le(std::string& s, uint32_t v, int bytes)
{
    for (int i = 0; i < bytes; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFFu));
}

using Files = std::vector<std::pair<std::string, std::string>>;

std::string stored_zip(const Files& files)
{
    std::string out;
    std::string central;
    for (const auto& [name, data] : files) {
        const auto offset = static_cast<uint32_t>(out.size());
        const uint32_t crc = crc32_of(data);
        const auto size = static_cast<uint32_t>(data.size());
        const auto name_len = static_cast<uint32_t>(name.size());
        out += "PK\x03\x04";
        for (const auto& [v, n] : {std::pair<uint32_t, int>{20, 2}, {0, 2}, {0, 2}, {0, 4}, {crc, 4}, {size, 4}, {size, 4}, {name_len, 2}, {0, 2}}) le(out, v, n);
        out += name + data;
        central += "PK\x01\x02";
        for (const auto& [v, n] : {std::pair<uint32_t, int>{20, 2}, {20, 2}, {0, 2}, {0, 2}, {0, 4}, {crc, 4}, {size, 4}, {size, 4}, {name_len, 2},
                                   {0, 2}, {0, 2}, {0, 2}, {0, 2}, {0, 4}, {offset, 4}})
            le(central, v, n);
        central += name;
    }
    const auto cd_offset = static_cast<uint32_t>(out.size());
    out += central;
    out += "PK\x05\x06";
    const auto count = static_cast<uint32_t>(files.size());
    for (const auto& [v, n] : {std::pair<uint32_t, int>{0, 2}, {0, 2}, {count, 2}, {count, 2}, {static_cast<uint32_t>(central.size()), 4}, {cd_offset, 4}, {0, 2}})
        le(out, v, n);
    return out;
}

struct EpubSpec {
    std::string container_path = "OEBPS/content.opf";
    std::string manifest_cover = R"(<item id="cover-img" href="images/cover.bmp" media-type="image/bmp" properties="cover-image"/>)";
    std::string metadata_extra;
    std::string cover_path = "OEBPS/images/cover.bmp";
    std::string container_override;
};

std::string make_epub(const EpubSpec& spec, const std::string& cover_bytes)
{
    const std::string container = !spec.container_override.empty()
        ? spec.container_override
        : R"(<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path=")" +
              spec.container_path + R"(" media-type="application/oebps-package+xml"/></rootfiles></container>)";
    const std::string opf =
        R"(<?xml version="1.0"?><package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">)"
        R"(<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">x</dc:identifier>)"
        R"(<dc:title>Synthetic EPUB</dc:title><dc:creator>Jane Doe</dc:creator><dc:language>VI-vn</dc:language>)" +
        spec.metadata_extra + R"(</metadata><manifest><item id="c1" href="ch1.xhtml" media-type="application/xhtml+xml"/>)" + spec.manifest_cover +
        R"(</manifest><spine><itemref idref="c1"/></spine></package>)";
    const std::string chapter = R"(<?xml version="1.0"?><html xmlns="http://www.w3.org/1999/xhtml"><head><title>t</title></head>)"
                                R"(<body><p>Hello synthetic epub.</p></body></html>)";
    return stored_zip({{"mimetype", "application/epub+zip"},
                       {"META-INF/container.xml", container},
                       {"OEBPS/content.opf", opf},
                       {"OEBPS/ch1.xhtml", chapter},
                       {spec.cover_path, cover_bytes}});
}

DocPtr open_bytes(const std::string& blob, const char* magic)
{
    eb_doc* d = nullptr;
    REQUIRE(eb_open_memory(blob.data(), blob.size(), magic, &d) == EB_OK);
    DocPtr doc(d);
    REQUIRE(eb_layout(d, 400, 600, 12, nullptr) == EB_OK);
    return doc;
}

BitmapPtr cover_of(eb_doc* d, int32_t side)
{
    eb_bitmap* raw = nullptr;
    REQUIRE(eb_render_cover(d, side, &raw) == EB_OK);
    REQUIRE(raw != nullptr);
    return BitmapPtr(raw);
}

int lum_at(const eb_bitmap& b, int x, int y)
{
    const size_t off = static_cast<size_t>(y) * static_cast<size_t>(b.stride) + static_cast<size_t>(x) * 4;
    return (b.bgra[off] + b.bgra[off + 1] + b.bgra[off + 2]) / 3;
}

std::string meta_of(eb_doc* d, const char* key)
{
    char buf[256] = {};
    size_t needed = 0;
    if (eb_meta(d, key, buf, sizeof buf, &needed) != EB_OK) return "<none>";
    return buf;
}

std::string base64(const std::string& in)
{
    static const std::string tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < in.size(); i += 3) {
        const uint32_t b0 = static_cast<unsigned char>(in[i]);
        const uint32_t b1 = i + 1 < in.size() ? static_cast<unsigned char>(in[i + 1]) : 0u;
        const uint32_t b2 = i + 2 < in.size() ? static_cast<unsigned char>(in[i + 2]) : 0u;
        const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(i + 1 < in.size() ? tbl[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < in.size() ? tbl[v & 63] : '=');
    }
    return out;
}

std::string make_fb2(const std::string& binary_text, const std::string& href)
{
    return R"(<?xml version="1.0" encoding="utf-8"?><FictionBook xmlns="http://www.gribuser.ru/xml/fictionbook/2.0" )"
           R"(xmlns:l="http://www.w3.org/1999/xlink"><description><title-info><genre>prose</genre>)"
           R"(<author><first-name>Anna</first-name><last-name>Nguyen</last-name></author><book-title>Synthetic FB2</book-title>)"
           R"(<coverpage><image l:href=")" + href + R"("/></coverpage><lang>Vi</lang></title-info></description>)"
           R"(<body><section><p>Hello synthetic fb2.</p></section></body><binary id="cov" content-type="image/bmp">)" + binary_text +
           "</binary></FictionBook>";
}

void expect_cover_ok(eb_doc* d)
{
    eb_bitmap* raw = nullptr;
    CHECK(eb_render_cover(d, 64, &raw) == EB_OK);
    eb_bitmap_free(raw);
}

} // namespace

TEST_CASE("eb_embed corpus EPUB and FB2 report language and embedded covers")
{
    const Lib lib;
    for (const auto& p : corpus("good")) {
        const std::string ext = p.extension().string();
        if (ext != ".epub" && ext != ".fb2") continue;
        INFO(p.string());
        const DocPtr d = open_doc(p);
        const std::string expected = p.filename().string() == "sample-vi.fb2" ? "vi" : "en";
        CHECK(meta_of(d.get(), "language") == expected);
        CHECK(meta_of(d.get(), "title") != "<none>");
        const BitmapPtr cover = cover_of(d.get(), 200);
        CHECK(std::max(cover->width, cover->height) <= 200);
        CHECK(not_uniform(*cover));
    }
}

TEST_CASE("eb_embed synthetic FB2 cover is upright and metadata is read")
{
    const Lib lib;
    for (const bool top_dark : {true, false}) {
        const DocPtr d = open_bytes(make_fb2(base64(make_bmp(top_dark)), "#cov"), ".fb2");
        CHECK(meta_of(d.get(), "language") == "vi");
        CHECK(meta_of(d.get(), "author") == "Anna Nguyen");
        const BitmapPtr cover = cover_of(d.get(), 64);
        REQUIRE(cover->width == 64);
        REQUIRE(cover->height == 64);
        CHECK((lum_at(*cover, 32, 3) < 64) == top_dark);
        CHECK((lum_at(*cover, 32, 60) > 192) == top_dark);
    }
}

TEST_CASE("eb_embed synthetic EPUB cover variants")
{
    const Lib lib;
    const std::string red = make_solid_bmp(220, 20, 20);
    const auto check_red = [&](const EpubSpec& spec, const char* what) {
        INFO(what);
        const DocPtr d = open_bytes(make_epub(spec, red), ".epub");
        CHECK(meta_of(d.get(), "language") == "vi");
        const BitmapPtr cover = cover_of(d.get(), 32);
        REQUIRE(cover->width == 32);
        const size_t off = static_cast<size_t>(16) * static_cast<size_t>(cover->stride) + size_t{16} * 4;
        CHECK(cover->bgra[off + 2] > 180);
        CHECK(cover->bgra[off + 1] < 70);
        CHECK(cover->bgra[off] < 70);
    };
    check_red(EpubSpec{}, "properties=cover-image");
    {
        EpubSpec s;
        s.manifest_cover = R"(<item id="img1" href="images/cover.bmp" media-type="image/bmp"/>)";
        s.metadata_extra = R"(<meta name="cover" content="img1"/>)";
        check_red(s, "meta name=cover");
    }
    {
        EpubSpec s;
        s.manifest_cover = R"(<item id="x" href="images/cover.bmp" media-type="image/bmp"/>)";
        check_red(s, "href contains cover");
    }
    {
        EpubSpec s;
        s.manifest_cover = R"(<item id="cover-img" href="../art/cover%2E.bmp" media-type="image/bmp" properties="cover-image"/>)";
        s.cover_path = "art/cover..bmp";
        s.container_path = "OEBPS/content.opf";
        check_red(s, "dotdot inside the archive and percent escapes");
    }
}

TEST_CASE("eb_embed malformed metadata falls back to page 0 without crashing")
{
    const Lib lib;
    const std::string red = make_solid_bmp(220, 20, 20);
    std::vector<std::string> blobs;
    {
        EpubSpec s;
        s.container_override = "<container><rootfiles><rootfile";
        blobs.push_back(make_epub(s, red));
    }
    {
        EpubSpec s;
        s.manifest_cover = R"(<item id="cover-img" href="../../escape.bmp" media-type="image/bmp" properties="cover-image"/>)";
        blobs.push_back(make_epub(s, red));
    }
    {
        EpubSpec s;
        s.manifest_cover = R"(<item id="cover-img" href="images/missing.bmp" media-type="image/bmp" properties="cover-image"/>)";
        blobs.push_back(make_epub(s, red));
    }
    blobs.push_back(make_epub(EpubSpec{}, "this is not an image"));
    for (const auto& blob : blobs) {
        eb_doc* raw = nullptr;
        if (eb_open_memory(blob.data(), blob.size(), ".epub", &raw) != EB_OK) continue;
        const DocPtr d(raw);
        REQUIRE(eb_layout(raw, 400, 600, 12, nullptr) == EB_OK);
        expect_cover_ok(d.get());
        char buf[16] = {};
        size_t needed = 0;
        (void)eb_meta(d.get(), "language", buf, sizeof buf, &needed);
    }
    for (const char* text : {"!!!not base64!!!", "", "QUJD"}) {
        const DocPtr d = open_bytes(make_fb2(text, "#cov"), ".fb2");
        expect_cover_ok(d.get());
    }
    const DocPtr missing = open_bytes(make_fb2(base64(make_bmp(true)), "#nothere"), ".fb2");
    expect_cover_ok(missing.get());
}
