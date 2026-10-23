#include <doctest/doctest.h>

#include "doc/TxtToFb2.h"

#include <windows.h>

#include <msxml6.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

namespace {

std::span<const std::byte> bytes_of(const std::string& s)
{
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

std::string read_file(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

std::string convert(const std::string& raw, std::wstring_view title = L"Fallback")
{
    auto r = eb::txt_to_fb2(bytes_of(raw), title);
    REQUIRE(r.has_value());
    return *r;
}

std::size_t count_of(const std::string& hay, const std::string& needle)
{
    std::size_t n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + needle.size())) {
        ++n;
    }
    return n;
}

bool well_formed(const std::string& utf8)
{
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IXMLDOMDocument2* doc = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(__uuidof(DOMDocument60), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&doc)))) {
        const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        std::wstring w(static_cast<std::size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), n);
        VARIANT_BOOL success = VARIANT_FALSE;
        BSTR bs = SysAllocStringLen(w.data(), static_cast<UINT>(w.size()));
        ok = n > 0 && SUCCEEDED(doc->loadXML(bs, &success)) && success == VARIANT_TRUE;
        SysFreeString(bs);
        doc->Release();
    }
    if (SUCCEEDED(init)) {
        CoUninitialize();
    }
    return ok;
}

std::string corpus_txt(const char* name)
{
    return read_file(std::filesystem::path(EB_CORPUS_DIR) / "good" / "txt" / name);
}

}

TEST_CASE("every corpus txt converts to well-formed FB2")
{
    for (const auto& e : std::filesystem::directory_iterator(std::filesystem::path(EB_CORPUS_DIR) / "good" / "txt")) {
        CAPTURE(e.path().string());
        const std::string out = convert(read_file(e.path()));
        CHECK(out.starts_with("<?xml"));
        CHECK(well_formed(out));
    }
}

TEST_CASE("truyen kieu encodings decode to the same text")
{
    const std::string ref = convert(corpus_txt("truyen-kieu-utf8.txt"));
    CHECK(ref.find("<lang>vi</lang>") != std::string::npos);
    CHECK(ref.find("Tr\xC4\x83m n\xC4\x83m trong c\xC3\xB5i ng\xC6\xB0\xE1\xBB\x9Di ta,") != std::string::npos);
    CHECK(convert(corpus_txt("truyen-kieu-utf8bom.txt")) == ref);
    CHECK(convert(corpus_txt("truyen-kieu-utf16le.txt")) == ref);
    // cp1258 can only encode Vietnamese via combining marks, so NFC must restore the same text.
    CHECK(convert(corpus_txt("truyen-kieu-cp1258.txt")) == ref);
}

TEST_CASE("utf16 without BOM and utf16be")
{
    const std::string le("T\0r\0u\0y\0\xE1\0n\0", 12);
    std::string be;
    for (std::size_t i = 0; i + 1 < le.size(); i += 2) {
        be.push_back(le[i + 1]);
        be.push_back(le[i]);
    }
    const std::string a = convert(le);
    CHECK(a.find("Truy\xC3\xA1n") != std::string::npos);
    CHECK(convert(be) == a);
    CHECK(convert(std::string("\xFE\xFF") + be) == a);
}

TEST_CASE("chapter headings are detected")
{
    const std::string raw = "Title line\n\nCh\xC6\xB0\xC6\xA1ng 1: M\xE1\xBB\x9F \xC4\x91\xE1\xBA\xA7u\n\nbody one\n\n"
                            "CH\xC6\xAF\xC6\xA0NG XII\n\nbody two\n\nChapter 3\n\nbody three\n\nIV.\n\nbody four\n";
    const std::string out = convert(raw);
    CHECK(count_of(out, "<section>") == 5);
    CHECK(out.find("<title><p>Ch\xC6\xB0\xC6\xA1ng 1: M\xE1\xBB\x9F \xC4\x91\xE1\xBA\xA7u</p></title>") != std::string::npos);
    CHECK(out.find("<title><p>CH\xC6\xAF\xC6\xA0NG XII</p></title>") != std::string::npos);
    CHECK(out.find("<title><p>Chapter 3</p></title>") != std::string::npos);
    CHECK(out.find("<title><p>IV.</p></title>") != std::string::npos);
    CHECK(out.find("<book-title>Title line</book-title>") != std::string::npos);
}

TEST_CASE("sentences starting with a keyword are not headings")
{
    const std::string raw = "Ph\xE1\xBA\xA7n l\xE1\xBB\x9Bn th\xE1\xBB\x9Di gian t\xC3\xB4i \xE1\xBB\x9F nh\xC3\xA0 \xC4\x91\xE1\xBB\x8D" "c s\xC3\xA1" "ch.\n\n"
                            "Part of the problem is that nobody cares.\n";
    const std::string out = convert(raw);
    CHECK(count_of(out, "<section>") == 1);
    CHECK(out.find("<title><p>1</p></title>") != std::string::npos);
}

TEST_CASE("no headings gives synthetic sections")
{
    std::string raw;
    for (int i = 0; i < 7000; ++i) {
        raw += "line " + std::to_string(i) + "\n";
    }
    const std::string out = convert(raw);
    CHECK(count_of(out, "<section>") == 3);
    CHECK(out.find("<title><p>3</p></title>") != std::string::npos);
}

TEST_CASE("paragraph splitting")
{
    const std::string no_blank = convert("alpha\nbeta\ngamma\n");
    CHECK(count_of(no_blank, "<p>") == 4);
    const std::string blank = convert("alpha\nbeta\n\ngamma\n");
    CHECK(blank.find("<p>alpha beta</p><p>gamma</p>") != std::string::npos);
}

TEST_CASE("xml escaping and control characters")
{
    const std::string out = convert(std::string("<script>&\"'\x01\x02 ok\x7F\r\nnext\rend\n"));
    CHECK(out.find("&lt;script&gt;&amp;&quot;&apos; ok") != std::string::npos);
    CHECK(out.find('\x01') == std::string::npos);
    CHECK(out.find("<script>") == std::string::npos);
    CHECK(count_of(out, "<p>") == 4);
    CHECK(well_formed(out));
}

TEST_CASE("fallback title, language and empty input")
{
    const std::string long_first(200, 'a');
    const std::string out = convert(long_first + "\nsecond\n", L"My <Book>");
    CHECK(out.find("<book-title>My &lt;Book&gt;</book-title>") != std::string::npos);
    CHECK(out.find("<lang>en</lang>") != std::string::npos);
    const std::string empty = convert("");
    CHECK(well_formed(empty));
    CHECK(count_of(empty, "<section>") == 1);
}

TEST_CASE("invalid utf16 surrogates still give well-formed output")
{
    const std::string raw("\xFF\xFE" "a\0\x00\xD8" "b\0", 8);
    CHECK(well_formed(convert(raw)));
}

TEST_CASE("size limit is enforced")
{
    const std::vector<std::byte> big(eb::kMaxTxtBytes + 1);
    auto r = eb::txt_to_fb2(big, L"x");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == eb::ErrorCode::Limit);
    const std::vector<std::byte> ok(eb::kMaxTxtBytes, std::byte{'a'});
    CHECK(eb::txt_to_fb2(ok, L"x").has_value());
}
