#include <doctest/doctest.h>

#include "doc/TxtToFb2.h"
#include "test_perf.h"

#include <windows.h>

#include <msxml6.h>
#include <psapi.h>

#include <chrono>
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
    // The cp1258 corpus file is lossy (unencodable letters were stored as '?'), so only the
    // leading text before the first lossy letter and the paragraph structure can be compared.
    const std::string cp = convert(corpus_txt("truyen-kieu-cp1258.txt"));
    CHECK(cp.find("Tr\xC4\x83m n\xC4\x83m trong c") != std::string::npos);
    CHECK(count_of(cp, "<p>") == count_of(ref, "<p>"));
    CHECK(cp.find("<lang>vi</lang>") != std::string::npos);
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

TEST_CASE("roman numerals and number words are validated")
{
    for (const char* bad : {"Part did not matter", "Part I think so", "Book mix", "DID", "MILD", "CIVIL", "MIX",
             "H\xE1\xBB\x93i \xC4\x91\xC3\xB3.", "H\xE1\xBB\x93i n\xC3\xA3y.", "Ph\xE1\xBA\xA7n \xC4\x91\xC3\xB4ng.",
             "Ph\xE1\xBA\xA7n l\xE1\xBB\x9Bn ng\xC6\xB0\xE1\xBB\x9Di d\xC3\xA2n \xE1\xBB\x9F \xC4\x91\xC3\xA2y."}) {
        CAPTURE(bad);
        CHECK(count_of(convert(std::string(bad) + "\n"), "<title><p>1</p></title>") == 1);
    }
    for (const char* good : {"H\xE1\xBB\x93i th\xE1\xBB\xA9 nh\xE1\xBA\xA5t", "Ch\xC6\xB0\xC6\xA1ng m\xE1\xBB\x99t",
             "Ch\xC6\xB0\xC6\xA1ng 12. Tr\xE1\xBB\x9F v\xE1\xBB\x81", "Chapter XIV. The End", "XLII", "7."}) {
        CAPTURE(good);
        CHECK(count_of(convert(std::string(good) + "\n"), "<title><p>1</p></title>") == 0);
    }
}

TEST_CASE("book title needs a following blank line or heading")
{
    CHECK(convert("Hello\nworld\n").find("<book-title>Fallback</book-title>") != std::string::npos);
    CHECK(convert("Hello\n\nworld\n").find("<book-title>Hello</book-title>") != std::string::npos);
    CHECK(convert("Hello\nChapter 1\nworld\n").find("<book-title>Hello</book-title>") != std::string::npos);
}

TEST_CASE("fallback title is sanitized and capped")
{
    std::wstring t = L"a\nb\x01";
    t.push_back(static_cast<wchar_t>(0xD800));
    CHECK(convert("x\nworld\n", t).find("<book-title>a b\xEF\xBF\xBD</book-title>") != std::string::npos);
    const std::string capped = convert("x\nworld\n", std::wstring(300, L'z'));
    CHECK(capped.find("<book-title>" + std::string(200, 'z') + "</book-title>") != std::string::npos);
}

TEST_CASE("cp1258 combining marks are composed to NFC")
{
    CHECK(convert(std::string("a\xCC")).find("<p>\xC3\xA0</p>") != std::string::npos);
    CHECK(convert(std::string("\xF5\xEC")).find("<p>\xE1\xBB\x9B</p>") != std::string::npos);
}

TEST_CASE("long paragraphs and sections are split")
{
    std::string one_line;
    for (int i = 0; i < 5000; ++i) {
        one_line += "word ";
    }
    CHECK(count_of(convert(one_line + "\n"), "<p>") >= 3);
    std::string big = "Chapter 1\n\n";
    for (int i = 0; i < 100; ++i) {
        big += std::string(5000, 'a') + "\n\n";
    }
    CHECK(count_of(convert(big), "<section>") >= 3);
}

TEST_CASE("32 MiB of short lines stays within the time and memory budget")
{
    std::string raw;
    raw.reserve(std::size_t{32} << 20);
    while (raw.size() < (std::size_t{32} << 20)) {
        raw += "a\n";
    }
    PROCESS_MEMORY_COUNTERS before{};
    GetProcessMemoryInfo(GetCurrentProcess(), &before, sizeof(before));
    const auto t0 = std::chrono::steady_clock::now();
    auto r = eb::txt_to_fb2(bytes_of(raw), L"x");
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(r.has_value());
    PROCESS_MEMORY_COUNTERS after{};
    GetProcessMemoryInfo(GetCurrentProcess(), &after, sizeof(after));
#ifdef NDEBUG
    CHECK(ms < ebtest::perf_budget(5000));
#else
    CHECK(ms < ebtest::perf_budget(60000));
#endif
#ifndef __SANITIZE_ADDRESS__
    CHECK(after.PeakWorkingSetSize - before.PeakWorkingSetSize < 4 * raw.size());
#endif
}
