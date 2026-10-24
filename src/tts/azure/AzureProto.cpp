#include "tts/azure/AzureProto.h"

#include "base/Text.h"
#include "tts/Utf8.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace eb {
namespace {

constexpr double kMinRate = 0.5;
constexpr double kMaxRate = 3.0;
constexpr size_t kMaxVoiceId = 64;
constexpr size_t kMaxJsonString = 512;
constexpr int kMaxJsonDepth = 32;

bool is_alnum(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool well_formed(const u8::Cp& c) noexcept
{
    if ((c.len == 2 && c.cp < 0x80) || (c.len == 3 && c.cp < 0x800) || (c.len == 4 && c.cp < 0x10000))
        return false;
    if (c.cp >= 0xD800 && c.cp <= 0xDFFF)
        return false;
    if (c.cp > 0x10FFFF || c.cp == 0xFFFE || c.cp == 0xFFFF)
        return false;
    return c.cp >= 0x20 || c.cp == '\t' || c.cp == '\n' || c.cp == '\r';
}

void append_utf8(std::string& out, char32_t cp)
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

enum class Tok : uint8_t { End, Obj, ObjEnd, Arr, ArrEnd, Colon, Comma, Str, Other, Bad };

class Lexer {
public:
    explicit Lexer(std::string_view s) noexcept : s_(s) {}

    Tok next(std::string& str)
    {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r'))
            ++pos_;
        if (pos_ >= s_.size())
            return Tok::End;
        switch (s_[pos_]) {
        case '{': ++pos_; return Tok::Obj;
        case '}': ++pos_; return Tok::ObjEnd;
        case '[': ++pos_; return Tok::Arr;
        case ']': ++pos_; return Tok::ArrEnd;
        case ':': ++pos_; return Tok::Colon;
        case ',': ++pos_; return Tok::Comma;
        case '"': return read_string(str) ? Tok::Str : Tok::Bad;
        default: skip_literal(); return Tok::Other;
        }
    }

private:
    void skip_literal() noexcept
    {
        do {
            ++pos_;
        } while (pos_ < s_.size() && std::string_view(" \t\r\n,:{}[]\"").find(s_[pos_]) == std::string_view::npos);
    }

    bool read_hex4(char32_t& v) noexcept
    {
        if (pos_ + 4 > s_.size())
            return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_++];
            const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0)
                return false;
            v = (v << 4) | static_cast<char32_t>(d);
        }
        return true;
    }

    bool read_unicode(std::string& out)
    {
        char32_t v = 0;
        if (!read_hex4(v))
            return false;
        if (v >= 0xDC00 && v <= 0xDFFF)
            v = 0xFFFD;
        else if (v >= 0xD800 && v <= 0xDBFF) {
            char32_t lo = 0;
            if (pos_ + 2 <= s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                pos_ += 2;
                if (!read_hex4(lo))
                    return false;
            }
            v = lo >= 0xDC00 && lo <= 0xDFFF ? 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00) : 0xFFFD;
        }
        append_utf8(out, v);
        return true;
    }

    bool read_escape(std::string& out)
    {
        if (pos_ >= s_.size())
            return false;
        const char e = s_[pos_++];
        switch (e) {
        case '"': case '\\': case '/': out.push_back(e); return true;
        case 'b': out.push_back('\b'); return true;
        case 'f': out.push_back('\f'); return true;
        case 'n': out.push_back('\n'); return true;
        case 'r': out.push_back('\r'); return true;
        case 't': out.push_back('\t'); return true;
        case 'u': return read_unicode(out);
        default: return false;
        }
    }

    bool read_string(std::string& out)
    {
        ++pos_;
        out.clear();
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"')
                return true;
            if (static_cast<unsigned char>(c) < 0x20)
                return false;
            if (c == '\\') {
                if (!read_escape(out))
                    return false;
            } else {
                out.push_back(c);
            }
            if (out.size() > kMaxJsonString)
                out.resize(kMaxJsonString);
        }
        return false;
    }

    std::string_view s_;
    size_t pos_ = 0;
};

struct Entry {
    std::string short_name, locale, local_name;
};

void assign_field(Entry& e, const std::string& key, const std::string& val)
{
    if (key == "ShortName")
        e.short_name = val;
    else if (key == "Locale")
        e.locale = val;
    else if (key == "LocalName")
        e.local_name = val;
}

bool wanted(const Entry& e, std::wstring_view keep, std::wstring& id)
{
    auto wide = utf8_to_wide(e.short_name);
    if (!wide || wide->empty() || !valid_azure_voice(e.short_name))
        return false;
    id = std::move(*wide);
    return e.locale == "vi-VN" || e.locale.starts_with("en-") || id == keep;
}

class VoiceCollector {
public:
    explicit VoiceCollector(std::wstring_view keep) noexcept : keep_(keep) {}

    bool feed(Tok t, const std::string& str)
    {
        switch (t) {
        case Tok::Obj:
        case Tok::Arr:
            if (++depth_ > kMaxJsonDepth)
                return false;
            if (t == Tok::Obj && depth_ == 2)
                cur_ = Entry{};
            break;
        case Tok::ObjEnd:
        case Tok::ArrEnd:
            if (t == Tok::ObjEnd && depth_ == 2)
                finish();
            if (--depth_ < 0)
                return false;
            break;
        case Tok::Colon:
            key_ = prev_ == Tok::Str ? last_str_ : std::string();
            break;
        case Tok::Str:
            if (depth_ == 2 && prev_ == Tok::Colon)
                assign_field(cur_, key_, str);
            last_str_ = str;
            break;
        default:
            break;
        }
        prev_ = t;
        return out.size() <= kAzureMaxVoices;
    }

    std::vector<Voice> out;

private:
    void finish()
    {
        std::wstring id;
        if (!wanted(cur_, keep_, id))
            return;
        auto lang = utf8_to_wide(cur_.locale);
        auto name = utf8_to_wide(cur_.local_name);
        out.push_back({std::move(id), name ? std::move(*name) : std::wstring(), lang ? std::move(*lang) : std::wstring()});
    }

    std::wstring_view keep_;
    int depth_ = 0;
    Tok prev_ = Tok::End;
    std::string key_, last_str_;
    Entry cur_;
};

}

bool valid_azure_region(std::string_view region) noexcept
{
    return region.size() >= 3 && region.size() <= 32
        && std::ranges::all_of(region, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); });
}

bool valid_azure_voice(std::string_view voice) noexcept
{
    if (voice.empty() || voice.size() > kMaxVoiceId)
        return false;
    const size_t d1 = voice.find('-');
    const size_t d2 = d1 == std::string_view::npos ? d1 : voice.find('-', d1 + 1);
    if (d2 == std::string_view::npos || d1 < 2 || d1 > 3 || d2 - d1 < 3 || d2 - d1 > 9 || voice.size() - d2 < 2)
        return false;
    return std::ranges::all_of(voice, [](char c) { return is_alnum(c) || c == '-'; })
        && std::ranges::all_of(voice.substr(0, d1), [](char c) { return c >= 'a' && c <= 'z'; });
}

int rate_percent(double rate) noexcept
{
    if (std::isnan(rate))
        rate = 1.0;
    return static_cast<int>(std::lround((std::clamp(rate, kMinRate, kMaxRate) - 1.0) * 100.0));
}

std::string xml_escape(std::string_view utf8)
{
    std::string out;
    out.reserve(utf8.size() + utf8.size() / 8);
    for (size_t i = 0; i < utf8.size();) {
        const auto c = u8::decode(utf8, i);
        const std::string_view raw = utf8.substr(i, c.len);
        i += c.len;
        if (c.cp == 0xFFFD && c.len == 1) {
            out += "\xEF\xBF\xBD";
        } else if (!well_formed(c)) {
            out.push_back(' ');
        } else if (c.cp == '&') {
            out += "&amp;";
        } else if (c.cp == '<') {
            out += "&lt;";
        } else if (c.cp == '>') {
            out += "&gt;";
        } else if (c.cp == '"') {
            out += "&quot;";
        } else if (c.cp == '\'') {
            out += "&apos;";
        } else {
            out += raw;
        }
    }
    return out;
}

std::string build_ssml(std::string_view utf8, std::string_view voice, int rate_pct)
{
    if (!valid_azure_voice(voice))
        return {};
    const size_t d2 = voice.find('-', voice.find('-') + 1);
    return std::format("<speak version=\"1.0\" xmlns=\"http://www.w3.org/2001/10/synthesis\" xml:lang=\"{}\">"
                       "<voice name=\"{}\"><prosody rate=\"{:+d}%\">{}</prosody></voice></speak>",
        voice.substr(0, d2), voice, rate_pct, xml_escape(utf8));
}

std::vector<Voice> parse_voices(std::string_view json, std::wstring_view keep)
{
    Lexer lx(json);
    VoiceCollector col(keep);
    std::string str;
    for (;;) {
        const Tok t = lx.next(str);
        if (t == Tok::End)
            break;
        if (t == Tok::Bad || !col.feed(t, str))
            return {};
    }
    return std::move(col.out);
}

}
