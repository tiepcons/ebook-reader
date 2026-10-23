#include "Segmenter.h"

#include "Utf8.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace eb {
namespace {

using u8::Cp;
using u8::decode;
using u8::is_space;

constexpr size_t kNpos = std::string_view::npos;

bool is_wide_terminator(char32_t c) noexcept
{
    return c == 0x3002 || c == 0xFF01 || c == 0xFF1F || c == 0xFF0E || (c >= 0x203C && c <= 0x2049 && c != 0x2040);
}

bool is_terminator(char32_t c) noexcept
{
    return c == '.' || c == '!' || c == '?' || c == 0x2026 || is_wide_terminator(c);
}

bool is_closer(char32_t c) noexcept
{
    return c == ')' || c == ']' || c == '}' || c == '"' || c == '\'' || c == 0xBB || c == 0x201D || c == 0x2019
        || c == 0x203A || c == 0x300D || c == 0x300F || c == 0xFF09;
}

size_t skip_space(std::string_view t, size_t i) noexcept
{
    while (i < t.size()) {
        const Cp c = decode(t, i);
        if (!is_space(c.cp))
            break;
        i += c.len;
    }
    return i;
}

size_t trim_end(std::string_view t, size_t b, size_t e) noexcept
{
    while (e > b) {
        size_t s = e - 1;
        while (s > b && u8::is_cont(t, s))
            --s;
        if (!is_space(decode(t, s).cp))
            break;
        e = s;
    }
    return e;
}

bool is_ascii_space(char c) noexcept
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

std::string lower_ascii(std::string_view s)
{
    std::string r(s);
    for (char& c : r)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return r;
}

bool is_dotted_initials(std::string_view tok) noexcept
{
    if (tok.size() < 3 || tok.size() % 2 == 0)
        return false;
    for (size_t i = 0; i < tok.size(); ++i) {
        const bool letter = (tok[i] >= 'a' && tok[i] <= 'z') || (tok[i] >= 'A' && tok[i] <= 'Z');
        if (i % 2 == 0 ? !letter : tok[i] != '.')
            return false;
    }
    return true;
}

bool is_list_marker(std::string_view tok, size_t tok_begin, size_t start) noexcept
{
    return tok_begin == start && tok.size() <= 3 && std::ranges::all_of(tok, [](char c) { return c >= '0' && c <= '9'; });
}

bool is_abbreviation(std::string_view t, size_t start, size_t dot)
{
    static constexpr std::array<std::string_view, 16> kWords = {"mr", "mrs", "ms", "dr", "st", "jr", "sr", "prof", "vs",
        "tp", "ts", "ths", "gs", "pgs", "bs", "mt"};
    size_t b = dot;
    while (b > start && !is_ascii_space(t[b - 1]) && t[b - 1] != '(' && t[b - 1] != '[' && t[b - 1] != '"'
        && t[b - 1] != '\'')
        --b;
    const std::string_view tok = t.substr(b, dot - b);
    if (tok.empty())
        return false;
    if (tok.size() == 1 && tok[0] >= 'A' && tok[0] <= 'Z')
        return tok[0] != 'I';
    if (is_dotted_initials(tok) || is_list_marker(tok, b, start))
        return true;
    const size_t dot_pos = tok.rfind('.');
    const std::string_view last = tok.substr(dot_pos == kNpos ? 0 : dot_pos + 1);
    return last.size() <= 4 && std::ranges::find(kWords, lower_ascii(last)) != kWords.end();
}

void push_chunks(std::vector<Sentence>& out, std::string_view t, size_t b, size_t e)
{
    while (b < e) {
        size_t i = b;
        size_t count = 0;
        size_t cut = kNpos;
        while (i < e && count < kMaxSentenceChars) {
            const Cp c = decode(t, i);
            if (is_space(c.cp))
                cut = i;
            else if ((c.cp == ',' || c.cp == ';' || c.cp == ':') && i + 1 < e && is_ascii_space(t[i + 1]))
                cut = i + 1;
            else if (c.cp == 0xFF0C || c.cp == 0x3001 || c.cp == 0xFF1B || c.cp == 0x3002)
                cut = i + c.len;
            i += c.len;
            ++count;
        }
        if (i >= e) {
            out.push_back({static_cast<uint32_t>(b), static_cast<uint32_t>(e)});
            return;
        }
        if (is_space(decode(t, i).cp))
            cut = i;
        if (cut == kNpos || trim_end(t, b, cut) <= b)
            cut = i;
        while (cut < e && u8::is_combining(decode(t, cut).cp))
            cut += decode(t, cut).len;
        out.push_back({static_cast<uint32_t>(b), static_cast<uint32_t>(trim_end(t, b, cut))});
        b = skip_space(t, cut);
    }
}

struct Splitter {
    std::string_view t;
    std::vector<Sentence> out;
    size_t start = 0;

    void emit(size_t end)
    {
        const size_t e = trim_end(t, start, end);
        if (e > start)
            push_chunks(out, t, start, e);
    }

    bool paragraph_break(size_t i) const noexcept
    {
        size_t j = i + 1;
        while (j < t.size() && t[j] != '\n' && is_ascii_space(t[j]))
            ++j;
        return j < t.size() && t[j] == '\n';
    }

    size_t run_end(size_t i, bool& wide) const noexcept
    {
        size_t j = i;
        while (j < t.size()) {
            const Cp c = decode(t, j);
            if (!is_terminator(c.cp))
                break;
            wide = wide || is_wide_terminator(c.cp);
            j += c.len;
        }
        while (j < t.size()) {
            const Cp c = decode(t, j);
            if (!is_closer(c.cp))
                break;
            j += c.len;
        }
        return j;
    }

    size_t at_terminator(size_t i)
    {
        bool wide = false;
        const size_t j = run_end(i, wide);
        if (j >= t.size()) {
            emit(j);
            start = j;
            return j;
        }
        const size_t k = skip_space(t, j);
        if (k >= t.size()) {
            emit(j);
            start = k;
            return k;
        }
        if (!wide && (k == j || u8::is_lower(decode(t, k).cp)))
            return j;
        const bool single_dot = t[i] == '.' && !is_terminator(decode(t, i + 1).cp);
        if (!wide && single_dot && is_abbreviation(t, start, i))
            return j;
        emit(j);
        start = k;
        return k;
    }

    void run()
    {
        start = skip_space(t, 0);
        size_t i = start;
        while (i < t.size()) {
            const Cp c = decode(t, i);
            if (c.cp == '\n' && paragraph_break(i)) {
                emit(i);
                i = start = skip_space(t, i);
            } else if (is_terminator(c.cp)) {
                i = at_terminator(i);
            } else {
                i += c.len;
            }
        }
        if (start < t.size())
            emit(t.size());
    }
};


bool is_roman(std::string_view s) noexcept
{
    auto place = [&](size_t i, char one, char five, char ten) {
        if (i + 1 < s.size() && s[i] == one && (s[i + 1] == five || s[i + 1] == ten))
            return i + 2;
        if (i < s.size() && s[i] == five)
            ++i;
        for (int k = 0; k < 3 && i < s.size() && s[i] == one; ++k)
            ++i;
        return i;
    };
    size_t i = 0;
    while (i < 3 && i < s.size() && s[i] == 'M')
        ++i;
    i = place(i, 'C', 'D', 'M');
    i = place(i, 'X', 'L', 'C');
    i = place(i, 'I', 'V', 'X');
    return !s.empty() && i == s.size();
}

bool is_number_token(std::string_view s, bool allow_lower)
{
    if (s.empty() || s.size() > 8)
        return false;
    if (s.size() <= 5 && std::ranges::all_of(s, [](char c) { return c >= '0' && c <= '9'; }))
        return true;
    if (!allow_lower && std::ranges::any_of(s, [](char c) { return c >= 'a' && c <= 'z'; }))
        return false;
    std::string up(s);
    for (char& c : up)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    return is_roman(up);
}

bool is_furniture_line(std::string_view line)
{
    auto dash = [](char c) { return is_ascii_space(c) || c == '-'; };
    while (!line.empty() && dash(line.front()))
        line.remove_prefix(1);
    while (!line.empty() && dash(line.back()))
        line.remove_suffix(1);
    bool prefixed = false;
    for (const std::string_view prefix : {"page ", "trang ", "pg. ", "p. "}) {
        if (line.size() > prefix.size() && lower_ascii(line.substr(0, prefix.size())) == prefix) {
            line.remove_prefix(prefix.size());
            prefixed = true;
            break;
        }
    }
    return is_number_token(line, prefixed);
}

bool line_at(std::string_view t, size_t at, bool from_start, std::string_view& line, size_t& edge)
{
    if (from_start) {
        const size_t b = skip_space(t, at);
        if (b >= t.size())
            return false;
        const size_t nl = t.find('\n', b);
        const size_t e = nl == kNpos ? t.size() : nl;
        line = t.substr(b, e - b);
        edge = e;
        return true;
    }
    const size_t e = trim_end(t, 0, at);
    if (e == 0)
        return false;
    const size_t nl = t.rfind('\n', e - 1);
    const size_t b = nl == kNpos ? 0 : nl + 1;
    line = t.substr(b, e - b);
    edge = b;
    return true;
}

}

std::vector<Sentence> split_sentences(std::string_view utf8) noexcept
{
    if (utf8.size() > std::numeric_limits<uint32_t>::max())
        return {};
    try {
        Splitter sp{utf8, {}, 0};
        sp.run();
        return std::move(sp.out);
    } catch (...) {
        return {};
    }
}

namespace {

bool joins_hyphen(const std::string& out, std::string_view next) noexcept
{
    if (out.size() < 3 || out.back() != '-' || next.empty() || next[0] < 'a' || next[0] > 'z')
        return false;
    size_t n = 0;
    size_t i = out.size() - 1;
    while (i > 0 && out[i - 1] != ' ') {
        --i;
        if (out[i] < 'a' || out[i] > 'z')
            return false;
        ++n;
    }
    return n >= 2;
}

}

std::string sentence_text(std::string_view utf8, Sentence s) noexcept
{
    if (s.begin > s.end || s.end > utf8.size())
        return {};
    try {
        const std::string_view t = utf8.substr(s.begin, s.end - s.begin);
        std::string out;
        out.reserve(t.size());
        size_t i = 0;
        while (i < t.size()) {
            const Cp c = decode(t, i);
            if (c.cp == 0xAD) {
                i += c.len;
            } else if (!is_space(c.cp)) {
                out.append(t.substr(i, c.len));
                i += c.len;
            } else {
                const size_t j = skip_space(t, i);
                const bool newline = t.substr(i, j - i).find('\n') != kNpos;
                if (newline && !out.empty() && out.back() == '-') {
                    if (joins_hyphen(out, t.substr(j)))
                        out.pop_back();
                } else if (!out.empty() && j < t.size()) {
                    out.push_back(' ');
                }
                i = j;
            }
        }
        return out;
    } catch (...) {
        return {};
    }
}

std::string_view strip_page_furniture(std::string_view utf8) noexcept
{
    try {
        std::string_view line;
        size_t edge = 0;
        std::string_view t = utf8.substr(0, trim_end(utf8, 0, utf8.size()));
        if (line_at(t, 0, true, line, edge) && is_furniture_line(line))
            t = t.substr(skip_space(t, edge));
        if (line_at(t, t.size(), false, line, edge) && is_furniture_line(line))
            t = t.substr(0, trim_end(t, 0, edge));
        return t;
    } catch (...) {
        return utf8;
    }
}

}
