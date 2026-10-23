#include "doc/TxtToFb2.h"

#include "base/Text.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace eb {
namespace {

constexpr std::size_t kSyntheticLines = 3000;
constexpr std::size_t kMaxHeading = 120;
constexpr std::size_t kMaxBookTitle = 80;
constexpr std::size_t kSniffBytes = 65536;
constexpr int kNormalizeRetries = 4;

bool is_space(wchar_t c) noexcept
{
    return c == L' ' || c == L'\t' || c == 0xA0 || c == 0x3000;
}

std::wstring_view trim(std::wstring_view s) noexcept
{
    while (!s.empty() && is_space(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && is_space(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

bool next_line(std::wstring_view text, std::size_t& pos, std::wstring_view& line) noexcept
{
    if (pos >= text.size()) {
        return false;
    }
    const std::size_t nl = text.find(L'\n', pos);
    const std::size_t end = nl == std::wstring_view::npos ? text.size() : nl;
    line = text.substr(pos, end - pos);
    pos = nl == std::wstring_view::npos ? text.size() : nl + 1;
    return true;
}

bool starts_with(std::span<const std::byte> raw, std::initializer_list<unsigned> bom) noexcept
{
    if (raw.size() < bom.size()) {
        return false;
    }
    std::size_t i = 0;
    for (const unsigned b : bom) {
        if (std::to_integer<unsigned>(raw[i++]) != b) {
            return false;
        }
    }
    return true;
}

std::wstring from_utf16(std::span<const std::byte> bytes, bool big_endian)
{
    std::wstring out;
    out.reserve(bytes.size() / 2);
    for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
        const unsigned a = std::to_integer<unsigned>(bytes[i]);
        const unsigned b = std::to_integer<unsigned>(bytes[i + 1]);
        out.push_back(static_cast<wchar_t>(big_endian ? (a << 8) | b : (b << 8) | a));
    }
    return out;
}

// Returns 0 for "not UTF-16", 1 for LE, 2 for BE.
int sniff_utf16(std::span<const std::byte> raw) noexcept
{
    const std::size_t n = std::min(raw.size(), kSniffBytes) & ~std::size_t{1};
    std::size_t even_zero = 0;
    std::size_t odd_zero = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (raw[i] == std::byte{0}) {
            ++((i & 1) ? odd_zero : even_zero);
        }
    }
    const std::size_t zeros = even_zero + odd_zero;
    if (n == 0 || zeros * 20 < n) {
        return 0;
    }
    if (odd_zero * 10 >= zeros * 9) {
        return 1;
    }
    return even_zero * 10 >= zeros * 9 ? 2 : 0;
}

Result<std::wstring> decode_cp1258(std::string_view s)
{
    if (s.empty()) {
        return std::wstring{};
    }
    const int len = static_cast<int>(s.size());
    const int n = MultiByteToWideChar(1258, 0, s.data(), len, nullptr, 0);
    if (n <= 0) {
        return fail(ErrorCode::Format, L"cp1258 decode failed");
    }
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    if (MultiByteToWideChar(1258, 0, s.data(), len, out.data(), n) != n) {
        return fail(ErrorCode::Format, L"cp1258 decode failed");
    }
    return out;
}

Result<std::wstring> decode_8bit(std::string_view s)
{
    if (auto w = utf8_to_wide(s)) {
        return w;
    }
    return decode_cp1258(s);
}

Result<std::wstring> decode_raw(std::span<const std::byte> raw)
{
    if (starts_with(raw, {0xEF, 0xBB, 0xBF})) {
        raw = raw.subspan(3);
        return decode_8bit({reinterpret_cast<const char*>(raw.data()), raw.size()});
    }
    if (starts_with(raw, {0xFF, 0xFE})) {
        return from_utf16(raw.subspan(2), false);
    }
    if (starts_with(raw, {0xFE, 0xFF})) {
        return from_utf16(raw.subspan(2), true);
    }
    if (const int u16 = sniff_utf16(raw); u16 != 0) {
        return from_utf16(raw, u16 == 2);
    }
    return decode_8bit({reinterpret_cast<const char*>(raw.data()), raw.size()});
}

bool is_high(wchar_t c) noexcept { return c >= 0xD800 && c <= 0xDBFF; }
bool is_low(wchar_t c) noexcept { return c >= 0xDC00 && c <= 0xDFFF; }

bool is_dropped(wchar_t c) noexcept
{
    return (c < 0x20 && c != L'\n' && c != L'\t') || (c >= 0x7F && c <= 0x9F) || c == 0xFFFE || c == 0xFFFF
        || c == 0xFEFF;
}

std::wstring sanitize(std::wstring_view in)
{
    std::wstring out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        wchar_t c = in[i];
        if (c == L'\r') {
            if (i + 1 < in.size() && in[i + 1] == L'\n') {
                continue;
            }
            c = L'\n';
        }
        if (is_high(c)) {
            if (i + 1 < in.size() && is_low(in[i + 1])) {
                out.push_back(c);
                out.push_back(in[++i]);
            }
            continue;
        }
        if (!is_low(c) && !is_dropped(c)) {
            out.push_back(c);
        }
    }
    return out;
}

Result<std::wstring> to_nfc(std::wstring s)
{
    if (s.empty()) {
        return s;
    }
    const int len = static_cast<int>(s.size());
    int cap = NormalizeString(NormalizationC, s.c_str(), len, nullptr, 0);
    for (int attempt = 0; attempt < kNormalizeRetries && cap > 0; ++attempt) {
        std::wstring out(static_cast<std::size_t>(cap), L'\0');
        const int r = NormalizeString(NormalizationC, s.c_str(), len, out.data(), cap);
        if (r > 0) {
            out.resize(static_cast<std::size_t>(r));
            return out;
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            break;
        }
        cap = -r;
    }
    return fail(ErrorCode::Format, L"NFC normalization failed");
}

bool is_digit(wchar_t c) noexcept { return c >= L'0' && c <= L'9'; }

bool all_of_chars(std::wstring_view s, std::wstring_view set) noexcept
{
    return !s.empty() && s.find_first_not_of(set) == std::wstring_view::npos;
}

bool is_bare_number(std::wstring_view t) noexcept
{
    if (!t.empty() && t.back() == L'.') {
        t.remove_suffix(1);
    }
    if (t.empty() || t.size() > 15) {
        return false;
    }
    return (t.size() <= 4 && all_of_chars(t, L"0123456789")) || all_of_chars(t, L"IVXLCDM");
}

constexpr std::array<std::wstring_view, 11> kKeywords{
    L"chương", L"chuong", L"hồi", L"hoi", L"quyển", L"quyen",
    L"phần", L"phan", L"chapter", L"part", L"book"};

bool is_alnum(wchar_t c) noexcept { return is_digit(c) || IsCharAlphaW(c) != FALSE; }

bool word_token_ok(std::wstring_view rest) noexcept
{
    rest = trim(rest);
    return rest.empty() || std::wstring_view(L":.-)]–—").find(rest.front()) != std::wstring_view::npos;
}

bool heading_after_keyword(std::wstring_view lower)
{
    std::size_t i = 0;
    while (i < lower.size() && is_space(lower[i])) {
        ++i;
    }
    if (i == 0) {
        return false;
    }
    std::size_t j = i;
    while (j < lower.size() && is_alnum(lower[j])) {
        ++j;
    }
    const std::wstring_view token = lower.substr(i, j - i);
    if (token.empty()) {
        return false;
    }
    if (all_of_chars(token, L"0123456789") || all_of_chars(token, L"ivxlcdm")) {
        return true;
    }
    const bool alpha_only = std::ranges::none_of(token, is_digit);
    return alpha_only && word_token_ok(lower.substr(j));
}

bool is_heading(std::wstring_view t)
{
    if (t.size() > kMaxHeading) {
        return false;
    }
    if (is_bare_number(t)) {
        return true;
    }
    std::wstring lower(t);
    CharLowerBuffW(lower.data(), static_cast<DWORD>(lower.size()));
    return std::ranges::any_of(kKeywords, [&](std::wstring_view kw) {
        return std::wstring_view(lower).starts_with(kw) && heading_after_keyword(std::wstring_view(lower).substr(kw.size()));
    });
}

struct Para {
    std::wstring text;
    std::size_t line;
};

struct Section {
    std::wstring title;
    bool titled = false;
    std::vector<Para> paras;
};

struct Parsed {
    std::vector<Section> sections;
    std::size_t headings = 0;
    std::wstring first_line;
};

bool has_blank_line(std::wstring_view text)
{
    std::size_t pos = 0;
    std::wstring_view line;
    while (next_line(text, pos, line)) {
        if (trim(line).empty()) {
            return true;
        }
    }
    return false;
}

void flush_block(Parsed& p, std::wstring& block, std::size_t line)
{
    if (!block.empty()) {
        p.sections.back().paras.push_back({std::move(block), line});
        block.clear();
    }
}

Parsed parse(std::wstring_view text)
{
    const bool per_line = !has_blank_line(text);
    Parsed p;
    p.sections.emplace_back();
    std::wstring block;
    std::size_t block_line = 0;
    std::size_t line_no = 0;
    std::size_t pos = 0;
    std::wstring_view line;
    while (next_line(text, pos, line)) {
        const std::wstring_view t = trim(line);
        ++line_no;
        const bool heading = !t.empty() && is_heading(t);
        if (p.first_line.empty() && !t.empty() && !heading) {
            p.first_line = std::wstring(t);
        }
        if (t.empty() || heading) {
            flush_block(p, block, block_line);
        }
        if (heading) {
            p.sections.push_back({std::wstring(t), true, {}});
            ++p.headings;
        } else if (!t.empty()) {
            if (block.empty()) {
                block_line = line_no;
            } else {
                block.push_back(L' ');
            }
            block.append(t);
            if (per_line) {
                flush_block(p, block, block_line);
            }
        }
    }
    flush_block(p, block, block_line);
    return p;
}

void split_synthetic(Parsed& p)
{
    std::vector<Para> paras = std::move(p.sections.front().paras);
    p.sections.clear();
    std::size_t bucket = static_cast<std::size_t>(-1);
    for (Para& para : paras) {
        const std::size_t b = para.line / kSyntheticLines;
        if (b != bucket) {
            bucket = b;
            p.sections.push_back({std::to_wstring(p.sections.size() + 1), true, {}});
        }
        p.sections.back().paras.push_back(std::move(para));
    }
}

void append_escaped(std::wstring& out, std::wstring_view s)
{
    for (const wchar_t c : s) {
        switch (c) {
        case L'&': out += L"&amp;"; break;
        case L'<': out += L"&lt;"; break;
        case L'>': out += L"&gt;"; break;
        case L'"': out += L"&quot;"; break;
        case L'\'': out += L"&apos;"; break;
        default: out.push_back(c);
        }
    }
}

void append_element(std::wstring& out, std::wstring_view tag, std::wstring_view text)
{
    out += L'<';
    out += tag;
    out += L'>';
    append_escaped(out, text);
    out += L"</";
    out += tag;
    out += L'>';
}

bool is_vietnamese_specific(wchar_t c) noexcept
{
    switch (c) {
    case 0x102: case 0x103: case 0xC2: case 0xE2: case 0x110: case 0x111: case 0xCA: case 0xEA:
    case 0xD4: case 0xF4: case 0x1A0: case 0x1A1: case 0x1AF: case 0x1B0: case 0x128: case 0x129:
    case 0x168: case 0x169:
        return true;
    default:
        return c >= 0x1EA0 && c <= 0x1EF9;
    }
}

bool looks_vietnamese(std::wstring_view text)
{
    std::size_t letters = 0;
    std::size_t viet = 0;
    std::array<WORD, 4096> types{};
    for (std::size_t off = 0; off < text.size(); off += types.size()) {
        const std::wstring_view chunk = text.substr(off, types.size());
        if (!GetStringTypeW(CT_CTYPE1, chunk.data(), static_cast<int>(chunk.size()), types.data())) {
            return false;
        }
        for (std::size_t i = 0; i < chunk.size(); ++i) {
            if (types[i] & C1_ALPHA) {
                ++letters;
                viet += is_vietnamese_specific(chunk[i]) ? 1 : 0;
            }
        }
    }
    return letters > 0 && viet * 100 >= letters;
}

void append_section(std::wstring& out, const Section& s)
{
    out += L"<section>";
    if (s.titled) {
        out += L"<title>";
        append_element(out, L"p", s.title);
        out += L"</title>";
    }
    for (const Para& p : s.paras) {
        append_element(out, L"p", p.text);
    }
    if (s.paras.empty()) {
        out += L"<empty-line/>";
    }
    out += L"</section>";
}

std::wstring build_fb2(const Parsed& p, std::wstring_view title, std::wstring_view lang)
{
    std::wstring out = L"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                       L"<FictionBook xmlns=\"http://www.gribuser.ru/xml/fictionbook/2.0\" "
                       L"xmlns:l=\"http://www.w3.org/1999/xlink\">"
                       L"<description><title-info>";
    append_element(out, L"book-title", title);
    append_element(out, L"lang", lang);
    out += L"</title-info></description><body>";
    for (const Section& s : p.sections) {
        append_section(out, s);
    }
    out += L"</body></FictionBook>\n";
    return out;
}

}

Result<std::string> txt_to_fb2(std::span<const std::byte> raw, std::wstring_view fallback_title) noexcept
try {
    if (raw.size() > kMaxTxtBytes) {
        return fail(ErrorCode::Limit, L"text file exceeds 64 MiB");
    }
    auto decoded = decode_raw(raw);
    if (!decoded) {
        return std::unexpected(std::move(decoded.error()));
    }
    auto text = to_nfc(sanitize(*decoded));
    if (!text) {
        return std::unexpected(std::move(text.error()));
    }
    decoded->clear();
    Parsed parsed = parse(*text);
    if (parsed.headings == 0) {
        split_synthetic(parsed);
    } else if (parsed.sections.front().paras.empty()) {
        parsed.sections.erase(parsed.sections.begin());
    }
    if (parsed.sections.empty()) {
        parsed.sections.emplace_back();
    }
    const bool use_line = !parsed.first_line.empty() && parsed.first_line.size() <= kMaxBookTitle;
    const std::wstring_view title = use_line ? std::wstring_view(parsed.first_line) : fallback_title;
    return wide_to_utf8(build_fb2(parsed, title, looks_vietnamese(*text) ? L"vi" : L"en"));
} catch (...) {
    return fail(ErrorCode::NoMemory, L"text conversion failed");
}

}
