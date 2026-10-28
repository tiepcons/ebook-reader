#include "tts/piper/PiperTts.h"

#include "tts/Utf8.h"

namespace eb {

namespace piper {

namespace {

constexpr char kHex[] = "0123456789abcdef";

void escape_ascii(std::string& out, char ch)
{
    switch (ch) {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
        if (static_cast<unsigned char>(ch) < 0x20) {
            out += "\\u00";
            out += kHex[(ch >> 4) & 0xF];
            out += kHex[ch & 0xF];
        } else {
            out += ch;
        }
    }
}

std::string_view string_after_key(std::string_view j, std::string_view key, size_t from, size_t limit)
{
    const std::string quoted = "\"" + std::string(key) + "\"";
    size_t p = j.find(quoted, from);
    if (p == std::string_view::npos || p >= limit)
        return {};
    p = j.find_first_not_of(" \t\r\n", p + quoted.size());
    if (p == std::string_view::npos || j[p] != ':')
        return {};
    p = j.find_first_not_of(" \t\r\n", p + 1);
    if (p == std::string_view::npos || j[p] != '"')
        return {};
    const size_t end = j.find('"', p + 1);
    if (end == std::string_view::npos)
        return {};
    return j.substr(p + 1, end - p - 1);
}

}

std::string json_escape(std::string_view s)
{
    std::string out;
    out.reserve(s.size() + 8);
    size_t i = 0;
    while (i < s.size()) {
        const auto c = u8::decode(s, i);
        if (c.cp == 0x2028 || c.cp == 0x2029) {
            out += c.cp == 0x2028 ? "\\u2028" : "\\u2029";
        } else if (c.len > 1) {
            out.append(s.substr(i, c.len));
        } else if (static_cast<unsigned char>(s[i]) >= 0x80) {
            out += "\\ufffd";
        } else {
            escape_ascii(out, s[i]);
        }
        i += c.len;
    }
    return out;
}

std::string request_line(std::string_view utf8, std::string_view wav_path_utf8)
{
    return "{\"text\":\"" + json_escape(utf8) + "\",\"output_file\":\"" + json_escape(wav_path_utf8) + "\"}";
}

std::wstring quote_arg(std::wstring_view a)
{
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
        return std::wstring(a);
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (const wchar_t c : a) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        out.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
        out.push_back(c);
        backslashes = 0;
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

std::wstring lang_from_model_json(std::string_view j)
{
    const size_t lang = j.find("\"language\"");
    if (lang == std::string_view::npos)
        return {};
    const size_t close = j.find('}', lang);
    const auto code = string_after_key(j, "code", lang, close == std::string_view::npos ? j.size() : close);
    if (code.empty() || code.size() > 16)
        return {};
    std::wstring out;
    for (const char c : code) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok)
            return {};
        out.push_back(c == '_' ? L'-' : static_cast<wchar_t>(c));
    }
    return out;
}

}

}
