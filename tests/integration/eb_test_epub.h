#ifndef EB_TEST_EPUB_H
#define EB_TEST_EPUB_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ebt {

inline void le(std::string& s, uint32_t v, int bytes)
{
    for (int i = 0; i < bytes; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFFu));
}

inline uint32_t crc32_of(const std::string& s)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (const unsigned char c : s) {
        crc ^= c;
        for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

inline std::string stored_zip(const std::vector<std::pair<std::string, std::string>>& files)
{
    std::string out;
    std::string central;
    for (const auto& [name, data] : files) {
        const auto offset = static_cast<uint32_t>(out.size());
        const uint32_t crc = crc32_of(data);
        const auto size = static_cast<uint32_t>(data.size());
        const auto name_len = static_cast<uint32_t>(name.size());
        out += "PK";
        for (const auto& [v, n] : {std::pair<uint32_t, int>{20, 2}, {0, 2}, {0, 2}, {0, 4}, {crc, 4}, {size, 4}, {size, 4}, {name_len, 2}, {0, 2}}) le(out, v, n);
        out += name + data;
        central += "PK";
        for (const auto& [v, n] : {std::pair<uint32_t, int>{20, 2}, {20, 2}, {0, 2}, {0, 2}, {0, 4}, {crc, 4}, {size, 4}, {size, 4}, {name_len, 2},
                                   {0, 2}, {0, 2}, {0, 2}, {0, 2}, {0, 4}, {offset, 4}})
            le(central, v, n);
        central += name;
    }
    const auto cd_offset = static_cast<uint32_t>(out.size());
    out += central;
    out += "PK";
    const auto count = static_cast<uint32_t>(files.size());
    for (const auto& [v, n] : {std::pair<uint32_t, int>{0, 2}, {0, 2}, {count, 2}, {count, 2}, {static_cast<uint32_t>(central.size()), 4}, {cd_offset, 4}, {0, 2}})
        le(out, v, n);
    return out;
}

struct EpubSpec {
    size_t chapters = 24;
    bool uniform = false; // every chapter has the same length
    bool toc = false;     // an EPUB 3 navigation document linking every third chapter with a fragment
};

inline std::string chapter_body(size_t index, bool uniform)
{
    std::string body = R"(<?xml version="1.0"?><html xmlns="http://www.w3.org/1999/xhtml"><head><title>t</title></head><body>)";
    const size_t paragraphs = uniform ? 12 : 6 + (index * 7) % 23;
    for (size_t p = 0; p < paragraphs; ++p) {
        body += "<p>Chapter " + std::to_string(index) + " paragraph " + std::to_string(p) +
                ". The quick brown fox jumps over the lazy dog again and again until the line wraps around the page.</p>";
    }
    return body + "</body></html>";
}

inline std::string nav_document(size_t chapters)
{
    std::string items;
    for (size_t i = 0; i < chapters; i += 3)
        items += "<li><a href=\"c" + std::to_string(i) + ".xhtml#top\">Chapter " + std::to_string(i) + "</a></li>";
    return R"(<?xml version="1.0"?><html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><head><title>n</title></head>)"
           R"(<body><nav epub:type="toc"><ol>)" + items + "</ol></nav></body></html>";
}

inline std::string multi_chapter_epub(const EpubSpec& spec = {})
{
    std::string manifest;
    std::string spine;
    std::vector<std::pair<std::string, std::string>> files;
    files.emplace_back("mimetype", "application/epub+zip");
    files.emplace_back("META-INF/container.xml",
                       R"(<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles>)"
                       R"(<rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>)");
    for (size_t i = 0; i < spec.chapters; ++i) {
        const std::string id = "c" + std::to_string(i);
        manifest.append(R"(<item id=")").append(id).append(R"(" href=")").append(id).append(R"(.xhtml" media-type="application/xhtml+xml"/>)");
        spine.append(R"(<itemref idref=")").append(id).append(R"("/>)");
    }
    if (spec.toc) manifest += R"(<item id="nav" href="nav.xhtml" properties="nav" media-type="application/xhtml+xml"/>)";
    files.emplace_back("OEBPS/content.opf",
                       R"(<?xml version="1.0"?><package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">)"
                       R"(<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">x</dc:identifier><dc:title>Cache</dc:title>)"
                       R"(<dc:language>en</dc:language></metadata><manifest>)" + manifest + "</manifest><spine>" + spine + "</spine></package>");
    for (size_t i = 0; i < spec.chapters; ++i) files.emplace_back("OEBPS/c" + std::to_string(i) + ".xhtml", chapter_body(i, spec.uniform));
    if (spec.toc) files.emplace_back("OEBPS/nav.xhtml", nav_document(spec.chapters));
    return stored_zip(files);
}

} // namespace ebt

#endif
