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

struct Entry {
    std::string name;
    std::string data;
    uint32_t method = 0;
    uint32_t usize = 0;
};

using Files = std::vector<Entry>;

std::string stored_zip(const Files& files)
{
    std::string out;
    std::string central;
    for (const auto& [name, data, method, usize] : files) {
        const auto offset = static_cast<uint32_t>(out.size());
        const uint32_t crc = method == 0 ? crc32_of(data) : 0;
        const auto size = static_cast<uint32_t>(data.size());
        const uint32_t full = method == 0 ? size : usize;
        const auto name_len = static_cast<uint32_t>(name.size());
        out += "PK\x03\x04";
        for (const auto& [v, n] : {std::pair<uint32_t, int>{20, 2}, {0, 2}, {method, 2}, {0, 4}, {crc, 4}, {size, 4}, {full, 4}, {name_len, 2}, {0, 2}}) le(out, v, n);
        out += name + data;
        central += "PK\x01\x02";
        for (const auto& [v, n] : {std::pair<uint32_t, int>{20, 2}, {20, 2}, {0, 2}, {method, 2}, {0, 4}, {crc, 4}, {size, 4}, {full, 4}, {name_len, 2},
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

constexpr uint32_t kBombMatches = 260000;
constexpr uint32_t kBombSize = 1 + kBombMatches * 258;

// Raw deflate stream (fixed Huffman) that inflates to kBombSize spaces: one literal, then 258-byte matches at distance 1.
std::string deflate_spaces()
{
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    const auto put = [&](uint32_t value, int count, bool msb_first) {
        for (int i = 0; i < count; ++i) {
            const uint32_t bit = msb_first ? (value >> (count - 1 - i)) & 1u : (value >> i) & 1u;
            acc |= bit << bits;
            if (++bits == 8) {
                out.push_back(static_cast<char>(acc));
                acc = 0;
                bits = 0;
            }
        }
    };
    put(1, 1, false);
    put(1, 2, false);
    put(0x30 + 0x20, 8, true);
    for (uint32_t i = 0; i < kBombMatches; ++i) {
        put(0xC5, 8, true);
        put(0, 5, true);
    }
    put(0, 7, true);
    put(0, 7, true);
    if (bits > 0) out.push_back(static_cast<char>(acc));
    return out;
}

struct EpubSpec {
    std::string container_path = "OEBPS/content.opf";
    std::string manifest_cover = R"(<item id="cover-img" href="images/cover.bmp" media-type="image/bmp" properties="cover-image"/>)";
    std::string metadata_extra;
    std::string cover_path = "OEBPS/images/cover.bmp";
    std::string container_override;
    bool bomb_cover = false;
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
    Entry cover{spec.cover_path, cover_bytes};
    if (spec.bomb_cover) cover = Entry{spec.cover_path, deflate_spaces(), 8, kBombSize};
    return stored_zip({{"mimetype", "application/epub+zip"},
                       {"META-INF/container.xml", container},
                       {"OEBPS/content.opf", opf},
                       {"OEBPS/ch1.xhtml", chapter},
                       cover});
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

TEST_CASE("eb_embed oversized cover entry is bounded and falls back to page 0")
{
    eb_limits lim{};
    lim.max_heap_bytes = size_t{64} << 20;
    const Lib lib(&lim);
    EpubSpec spec;
    spec.bomb_cover = true;
    const std::string blob = make_epub(spec, std::string());
    REQUIRE(blob.size() < (size_t{1} << 20));
    const DocPtr d = open_bytes(blob, ".epub");
    expect_cover_ok(d.get());
    CHECK(meta_of(d.get(), "language") == "vi");
}

namespace {

std::string linked_epub()
{
    const std::string container =
        R"(<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles>)"
        R"(<rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>)";
    const std::string opf =
        R"(<?xml version="1.0"?><package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">)"
        R"(<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">x</dc:identifier><dc:title>Links</dc:title>)"
        R"(<dc:language>en</dc:language></metadata><manifest><item id="c1" href="ch1.xhtml" media-type="application/xhtml+xml"/>)"
        R"(<item id="c2" href="ch2.xhtml" media-type="application/xhtml+xml"/><item id="c3" href="ch3.xhtml" media-type="application/xhtml+xml"/>)"
        R"(</manifest><spine><itemref idref="c1"/><itemref idref="c2"/><itemref idref="c3"/></spine></package>)";
    const auto page = [](const std::string& body) {
        return R"(<?xml version="1.0"?><html xmlns="http://www.w3.org/1999/xhtml"><head><title>t</title></head><body>)" + body + "</body></html>";
    };
    const std::string toc = page(R"(<p id="top"><a href="ch2.xhtml">Chapter two</a></p><p><a href="ch3.xhtml#sec">Section</a></p>)"
                                 R"(<p><a href="http://example.com/x">Web</a></p><p><a href="#top">Top</a></p>)");
    return stored_zip({{"mimetype", "application/epub+zip"},
                       {"META-INF/container.xml", container},
                       {"OEBPS/content.opf", opf},
                       {"OEBPS/ch1.xhtml", toc},
                       {"OEBPS/ch2.xhtml", page("<p>Second chapter.</p>")},
                       {"OEBPS/ch3.xhtml", page(R"(<p>Third chapter.</p><p id="sec">Section text.</p>)")}});
}

struct LinksDeleter {
    void operator()(eb_link* l) const { eb_links_free(l); }
};

}

TEST_CASE("eb_page_links returns internal links only, with resolvable targets")
{
    const Lib lib;
    const DocPtr d = open_bytes(linked_epub(), ".epub");
    int32_t pages = 0;
    REQUIRE(eb_page_count(d.get(), &pages) == EB_OK);
    REQUIRE(pages >= 3);
    eb_link* raw = nullptr;
    int32_t n = 0;
    REQUIRE(eb_page_links(d.get(), 0, &raw, &n) == EB_OK);
    const std::unique_ptr<eb_link, LinksDeleter> links(raw);
    CHECK(n == 3);
    int32_t beyond = 0;
    for (int32_t i = 0; i < n; ++i) {
        const eb_link& l = links.get()[i];
        CHECK(l.area.x1 > l.area.x0);
        int32_t back = -1;
        REQUIRE(eb_page_of_bookmark(d.get(), l.target, &back) == EB_OK);
        CHECK(back == l.page);
        if (l.page > 0) ++beyond;
    }
    CHECK(beyond == 2);
    eb_link dummy{};
    eb_link* none = &dummy;
    int32_t zero = -1;
    REQUIRE(eb_page_links(d.get(), pages - 1, &none, &zero) == EB_OK);
    CHECK(zero == 0);
    CHECK(none == nullptr);
    CHECK(eb_page_links(d.get(), pages, &none, &zero) == EB_E_RANGE);
    CHECK(eb_page_links(nullptr, 0, &none, &zero) == EB_E_ARG);
}

TEST_CASE("eb_page_links and eb_page_text report boxes relative to a non-zero page origin")
{
    const Lib lib;
    const std::string stream = "BT /F1 12 Tf 110 280 Td (Hello) Tj ET";
    const std::string pdf =
        "%PDF-1.4\n"
        "1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n"
        "2 0 obj<</Type/Pages/Kids[3 0 R 4 0 R]/Count 2/MediaBox[100 100 300 300]>>endobj\n"
        "3 0 obj<</Type/Page/Parent 2 0 R/Contents 5 0 R/Resources<</Font<</F1 6 0 R>>>>/Annots[7 0 R]>>endobj\n"
        "4 0 obj<</Type/Page/Parent 2 0 R>>endobj\n"
        "5 0 obj<</Length " + std::to_string(stream.size()) + ">>stream\n" + stream + "\nendstream endobj\n"
        "6 0 obj<</Type/Font/Subtype/Type1/BaseFont/Helvetica>>endobj\n"
        "7 0 obj<</Type/Annot/Subtype/Link/Rect[110 110 150 150]/Border[0 0 0]/Dest[4 0 R/Fit]>>endobj\n"
        "trailer<</Root 1 0 R/Size 8>>\n";
    const DocPtr d = open_bytes(pdf, ".pdf");
    eb_text* t = nullptr;
    REQUIRE(eb_page_text(d.get(), 0, &t) == EB_OK);
    const TextPtr text(t);
    REQUIRE(text->char_count > 0);
    for (int32_t i = 0; i < text->char_count; ++i) {
        CHECK(text->box[i].x0 >= 0);
        CHECK(text->box[i].x1 <= 200);
        CHECK(text->box[i].y1 <= 200);
    }
    eb_link* raw = nullptr;
    int32_t n = 0;
    REQUIRE(eb_page_links(d.get(), 0, &raw, &n) == EB_OK);
    const std::unique_ptr<eb_link, LinksDeleter> links(raw);
    REQUIRE(n == 1);
    CHECK(links.get()[0].area.x0 == doctest::Approx(10.0F));
    CHECK(links.get()[0].area.x1 == doctest::Approx(50.0F));
    CHECK(links.get()[0].page == 1);
}
