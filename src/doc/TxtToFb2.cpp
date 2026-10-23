#include "doc/TxtToFb2.h"

#include "doc/TxtDecode.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>

namespace eb {
namespace {

constexpr std::size_t kSyntheticLines = 3000;
constexpr std::size_t kMaxSectionChars = 200000;
constexpr std::size_t kSoftParaChars = 8000;
constexpr std::size_t kHardParaChars = 16000;
constexpr std::size_t kMaxHeading = 120;
constexpr std::size_t kMaxBookTitle = 80;
constexpr std::size_t kMaxFallbackTitle = 200;
constexpr wchar_t kNoChar = L'\1';

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

bool is_digit(wchar_t c) noexcept { return c >= L'0' && c <= L'9'; }

bool all_of_chars(std::wstring_view s, std::wstring_view set) noexcept
{
    return !s.empty() && s.find_first_not_of(set) == std::wstring_view::npos;
}

void eat_roman_digit(std::wstring_view s, std::size_t& i, wchar_t one, wchar_t five, wchar_t ten) noexcept
{
    const auto at = [&](std::size_t k) { return k < s.size() ? s[k] : L'\0'; };
    if (at(i) == one && (at(i + 1) == ten || at(i + 1) == five)) {
        i += 2;
        return;
    }
    if (at(i) == five) {
        ++i;
    }
    for (int n = 0; n < 3 && at(i) == one; ++n) {
        ++i;
    }
}

// Uppercase roman numerals below 1000 (no M: chapter numbers never need it, and it avoids "MIX", "MILD").
bool is_roman(std::wstring_view s) noexcept
{
    std::size_t i = 0;
    eat_roman_digit(s, i, L'C', L'D', kNoChar);
    eat_roman_digit(s, i, L'X', L'L', L'C');
    eat_roman_digit(s, i, L'I', L'V', L'X');
    return !s.empty() && i == s.size();
}

bool is_bare_number(std::wstring_view t) noexcept
{
    if (!t.empty() && t.back() == L'.') {
        t.remove_suffix(1);
    }
    return (t.size() <= 4 && all_of_chars(t, L"0123456789")) || is_roman(t);
}

constexpr std::array<std::wstring_view, 11> kKeywords{
    L"chương", L"chuong", L"hồi", L"hoi", L"quyển", L"quyen",
    L"phần", L"phan", L"chapter", L"part", L"book"};

constexpr std::array<std::wstring_view, 52> kNumberWords{
    L"one", L"two", L"three", L"four", L"five", L"six", L"seven", L"eight", L"nine", L"ten",
    L"eleven", L"twelve", L"thirteen", L"fourteen", L"fifteen", L"sixteen", L"seventeen", L"eighteen",
    L"nineteen", L"twenty", L"first", L"second", L"third", L"fourth", L"fifth", L"sixth", L"seventh",
    L"eighth", L"ninth", L"tenth",
    L"một", L"hai", L"ba", L"bốn", L"năm", L"sáu", L"bảy", L"tám",
    L"chín", L"mười", L"nhất", L"nhì",
    L"mot", L"bon", L"nam", L"sau", L"bay", L"tam", L"chin", L"muoi", L"nhat", L"nhi"};

constexpr std::wstring_view kThu = L"thứ";

bool is_alnum(wchar_t c) noexcept { return is_digit(c) || IsCharAlphaW(c) != FALSE; }

bool separator_follows(std::wstring_view rest, std::wstring_view seps, bool allow_end) noexcept
{
    rest = trim(rest);
    return rest.empty() ? allow_end : seps.find(rest.front()) != std::wstring_view::npos;
}

struct Span {
    std::size_t begin;
    std::size_t end;
};

Span read_token(std::wstring_view s, std::size_t pos) noexcept
{
    while (pos < s.size() && is_space(s[pos])) {
        ++pos;
    }
    std::size_t end = pos;
    while (end < s.size() && is_alnum(s[end])) {
        ++end;
    }
    return {pos, end};
}

bool heading_after_keyword(std::wstring_view orig, std::wstring_view lower, std::size_t pos)
{
    if (pos >= lower.size() || !is_space(lower[pos])) {
        return false;
    }
    Span tk = read_token(lower, pos);
    if (tk.begin == tk.end) {
        return false;
    }
    if (lower.substr(tk.begin, tk.end - tk.begin) == kThu) {
        tk = read_token(lower, tk.end);
        if (tk.begin == tk.end) {
            return false;
        }
    }
    const std::wstring_view token = lower.substr(tk.begin, tk.end - tk.begin);
    const std::wstring_view rest = lower.substr(tk.end);
    if (all_of_chars(token, L"0123456789")) {
        return true;
    }
    if (is_roman(orig.substr(tk.begin, tk.end - tk.begin))) {
        return separator_follows(rest, L":.-)–—", true);
    }
    if (std::ranges::find(kNumberWords, token) != kNumberWords.end()) {
        return true;
    }
    return separator_follows(rest, L":-–—", false);
}

bool may_start_heading(wchar_t c) noexcept
{
    return is_digit(c) || std::wstring_view(L"cChHqQpPbBIVXLD").find(c) != std::wstring_view::npos;
}

bool is_heading(std::wstring_view t)
{
    if (t.empty() || t.size() > kMaxHeading || !may_start_heading(t.front())) {
        return false;
    }
    if (is_bare_number(t)) {
        return true;
    }
    std::array<wchar_t, kMaxHeading> buf{};
    std::ranges::copy(t, buf.begin());
    CharLowerBuffW(buf.data(), static_cast<DWORD>(t.size()));
    const std::wstring_view lower(buf.data(), t.size());
    return std::ranges::any_of(kKeywords, [&](std::wstring_view kw) {
        return lower.starts_with(kw) && heading_after_keyword(t, lower, kw.size());
    });
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

struct Scan {
    std::size_t headings = 0;
    bool has_blank = false;
    std::wstring_view title;
};

// The book title is the first line when it is short, not a heading, and set apart by a blank line or heading.
Scan scan(std::wstring_view text)
{
    Scan sc;
    enum class Title : std::uint8_t { Seeking, Pending, Done } state = Title::Seeking;
    std::wstring_view candidate;
    std::size_t pos = 0;
    std::wstring_view line;
    while (next_line(text, pos, line)) {
        const std::wstring_view t = trim(line);
        const bool heading = is_heading(t);
        sc.headings += heading ? 1 : 0;
        sc.has_blank = sc.has_blank || t.empty();
        if (state == Title::Pending) {
            if (t.empty() || heading) {
                sc.title = candidate;
            }
            state = Title::Done;
        } else if (state == Title::Seeking && !t.empty()) {
            state = !heading && t.size() <= kMaxBookTitle ? Title::Pending : Title::Done;
            candidate = t;
        }
    }
    return sc;
}

void append_utf8(std::string& out, std::uint32_t cp)
{
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

void append_text(std::string& out, std::wstring_view s)
{
    for (std::size_t i = 0; i < s.size(); ++i) {
        const wchar_t c = s[i];
        switch (c) {
        case L'&': out += "&amp;"; break;
        case L'<': out += "&lt;"; break;
        case L'>': out += "&gt;"; break;
        case L'"': out += "&quot;"; break;
        case L'\'': out += "&apos;"; break;
        default:
            if (is_high(c) && i + 1 < s.size() && is_low(s[i + 1])) {
                const std::uint32_t lo = static_cast<std::uint32_t>(s[++i]) - 0xDC00;
                append_utf8(out, 0x10000 + ((static_cast<std::uint32_t>(c) - 0xD800) << 10) + lo);
            } else {
                append_utf8(out, is_high(c) || is_low(c) ? 0xFFFD : static_cast<std::uint32_t>(c));
            }
        }
    }
}

std::wstring clean_title(std::wstring_view t)
{
    std::wstring out;
    for (std::size_t i = 0; i < t.size() && out.size() < kMaxFallbackTitle; ++i) {
        const wchar_t c = t[i];
        if (c == L'\n' || c == L'\r' || c == L'\t') {
            out.push_back(L' ');
        } else if (is_high(c) && i + 1 < t.size() && is_low(t[i + 1]) && out.size() + 2 <= kMaxFallbackTitle) {
            out.push_back(c);
            out.push_back(t[++i]);
        } else if (is_high(c) || is_low(c)) {
            out.push_back(L'�');
        } else if (!is_control(c)) {
            out.push_back(c);
        }
    }
    return out;
}

struct Emitter {
    std::string out;
    bool synthetic = false;
    bool section_open = false;
    bool section_empty = true;
    bool para_open = false;
    std::size_t line = 0;
    std::size_t section_line = 0;
    std::size_t section_chars = 0;
    std::size_t para_chars = 0;
    std::size_t synthetic_count = 0;
};

void close_paragraph(Emitter& e)
{
    if (e.para_open) {
        e.out += "</p>";
        e.para_open = false;
    }
}

void close_section(Emitter& e)
{
    close_paragraph(e);
    if (e.section_open) {
        e.out += e.section_empty ? "<empty-line/></section>" : "</section>";
        e.section_open = false;
    }
}

void open_section(Emitter& e, std::wstring_view title)
{
    e.out += "<section>";
    if (!title.empty()) {
        e.out += "<title><p>";
        append_text(e.out, title);
        e.out += "</p></title>";
    }
    e.section_open = true;
    e.section_empty = true;
    e.section_chars = 0;
    e.section_line = e.line;
}

void begin_paragraph(Emitter& e)
{
    const bool too_long = e.section_chars >= kMaxSectionChars
        || (e.synthetic && e.line - e.section_line >= kSyntheticLines);
    if (e.section_open && too_long) {
        close_section(e);
    }
    if (!e.section_open) {
        open_section(e, e.synthetic ? std::to_wstring(++e.synthetic_count) : std::wstring());
    }
    e.out += "<p>";
    e.para_open = true;
    e.para_chars = 0;
    e.section_empty = false;
}

void add_text(Emitter& e, std::wstring_view s)
{
    append_text(e.out, s);
    e.para_chars += s.size();
    e.section_chars += s.size();
}

std::size_t find_cut(std::wstring_view t) noexcept
{
    const std::size_t limit = std::min(t.size(), kHardParaChars);
    for (std::size_t i = kSoftParaChars; i < limit; ++i) {
        if (is_space(t[i])) {
            return i;
        }
    }
    if (t.size() <= kHardParaChars) {
        return 0;
    }
    return is_high(t[kHardParaChars - 1]) ? kHardParaChars - 1 : kHardParaChars;
}

void add_line(Emitter& e, std::wstring_view t, bool per_line)
{
    if (e.para_open && e.para_chars >= kSoftParaChars) {
        close_paragraph(e);
    }
    if (!e.para_open) {
        begin_paragraph(e);
    } else {
        e.out += ' ';
    }
    for (std::size_t cut = t.size() > kSoftParaChars ? find_cut(t) : 0; cut != 0;
         cut = t.size() > kSoftParaChars ? find_cut(t) : 0) {
        add_text(e, t.substr(0, cut));
        close_paragraph(e);
        t = trim(t.substr(cut));
        if (t.empty()) {
            return;
        }
        begin_paragraph(e);
    }
    add_text(e, t);
    if (per_line) {
        close_paragraph(e);
    }
}

void emit_body(Emitter& e, std::wstring_view text, bool per_line)
{
    std::size_t pos = 0;
    std::wstring_view line;
    while (next_line(text, pos, line)) {
        const std::wstring_view t = trim(line);
        ++e.line;
        if (t.empty()) {
            close_paragraph(e);
        } else if (is_heading(t)) {
            close_section(e);
            open_section(e, t);
        } else {
            add_line(e, t, per_line);
        }
    }
    close_section(e);
    if (e.out.find("<section>") == std::string::npos) {
        e.out += "<section><empty-line/></section>";
    }
}

void emit_header(std::string& out, std::wstring_view title, bool vietnamese)
{
    out += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<FictionBook xmlns=\"http://www.gribuser.ru/xml/fictionbook/2.0\" "
           "xmlns:l=\"http://www.w3.org/1999/xlink\"><description><title-info><book-title>";
    append_text(out, title);
    out += vietnamese ? "</book-title><lang>vi</lang>" : "</book-title><lang>en</lang>";
    out += "</title-info></description><body>";
}

Result<std::string> convert(std::span<const std::byte> raw, std::wstring_view fallback_title)
{
    auto decoded = decode_text(raw);
    if (!decoded) {
        return std::unexpected(std::move(decoded.error()));
    }
    const std::wstring& text = *decoded;
    const Scan sc = scan(text);
    const std::wstring fallback = clean_title(fallback_title);
    Emitter e;
    e.synthetic = sc.headings == 0;
    e.out.reserve(raw.size() + raw.size() / 3 + 1024);
    emit_header(e.out, sc.title.empty() ? std::wstring_view(fallback) : sc.title, looks_vietnamese(text));
    emit_body(e, text, !sc.has_blank);
    e.out += "</body></FictionBook>\n";
    return std::move(e.out);
}

}

Result<std::string> txt_to_fb2(std::span<const std::byte> raw, std::wstring_view fallback_title) noexcept
try {
    if (raw.size() > kMaxTxtBytes) {
        return fail(ErrorCode::Limit, L"text file exceeds 64 MiB");
    }
    return convert(raw, fallback_title);
} catch (...) {
    return fail(ErrorCode::NoMemory, L"text conversion failed");
}

}
