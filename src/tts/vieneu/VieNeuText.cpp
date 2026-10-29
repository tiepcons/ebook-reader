#include "tts/vieneu/VieNeuText.h"

#include "base/FileIo.h"
#include "base/Text.h"
#include "tts/Utf8.h"
#include "tts/piper/PiperProc.h"
#include "tts/piper/PiperTts.h"

#include <algorithm>

namespace eb::vieneu {

namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxHeadBytes = 64;
constexpr size_t kMaxHeadWords = 5;
constexpr size_t kMaxChunks = 16;
constexpr size_t kWavHeaderBytes = 44;
constexpr size_t kMaxPresetDirs = 512;
constexpr size_t kMaxPresets = 256;
constexpr size_t kMaxIdChars = 64;
constexpr size_t kMaxNameBytes = 64;
constexpr size_t kManifestMax = size_t{64} * 1024;
constexpr uintmax_t kEmbMin = 256;
constexpr uintmax_t kEmbMax = size_t{64} * 1024;
constexpr uintmax_t kCodesMin = 64;
constexpr uintmax_t kCodesMax = size_t{1024} * 1024;

bool is_ignorable(char32_t c) noexcept
{
    return (c >= 0x200B && c <= 0x200F) || c == 0x2060 || c == 0xFEFF;
}

bool is_separator(char32_t c) noexcept
{
    return c < 0x20 || (c >= 0x7F && c <= 0x9F) || c == 0xFFFD || u8::is_space(c);
}

std::string_view trim(std::string_view s) noexcept
{
    while (!s.empty() && (s.front() == ' ' || (s.front() >= '\t' && s.front() <= '\r')))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || (s.back() >= '\t' && s.back() <= '\r')))
        s.remove_suffix(1);
    return s;
}

bool ends_sentence(char32_t c) noexcept
{
    return c == '.' || c == '!' || c == '?' || c == ';' || c == 0x2026;
}

uint32_t rd32(std::string_view b, size_t o) noexcept
{
    uint32_t v = 0;
    for (size_t i = 0; i < 4; ++i)
        v |= static_cast<uint32_t>(static_cast<unsigned char>(b[o + i])) << (8 * i);
    return v;
}

uint16_t rd16(std::string_view b, size_t o) noexcept
{
    return static_cast<uint16_t>(static_cast<unsigned char>(b[o]) | (static_cast<unsigned char>(b[o + 1]) << 8));
}

void put32(std::string& s, uint32_t v)
{
    for (size_t i = 0; i < 4; ++i)
        s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

void put16(std::string& s, uint16_t v)
{
    s.push_back(static_cast<char>(v & 0xFF));
    s.push_back(static_cast<char>(v >> 8));
}

bool read_format(std::string_view b, size_t body, size_t size, WavInfo& w) noexcept
{
    if (size < 16 || body + 16 > b.size())
        return false;
    const uint16_t channels = rd16(b, body + 2);
    const uint32_t rate = rd32(b, body + 4);
    if (rd16(b, body) != 1 || rd16(b, body + 14) != 16 || channels < 1 || channels > 2 || rate < 8000 || rate > 192000)
        return false;
    w.rate = rate;
    w.channels = channels;
    return true;
}

std::string header(const WavInfo& w, size_t data_len)
{
    std::string h = "RIFF";
    put32(h, static_cast<uint32_t>(kWavHeaderBytes - 8 + data_len));
    h += "WAVEfmt ";
    put32(h, 16);
    put16(h, 1);
    put16(h, w.channels);
    put32(h, w.rate);
    put32(h, w.rate * w.channels * 2);
    put16(h, static_cast<uint16_t>(w.channels * 2));
    put16(h, 16);
    h += "data";
    put32(h, static_cast<uint32_t>(data_len));
    return h;
}

std::string read_head(const fs::path& p)
{
    return eb::read_head(p, kManifestMax);
}

bool plain_id(const std::wstring& id) noexcept
{
    return !id.empty() && id.size() <= kMaxIdChars
        && std::ranges::all_of(id, [](wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'_' || c == L'-'; });
}

bool sized_file(const fs::path& p, uintmax_t lo, uintmax_t hi) noexcept
{
    std::error_code ec;
    if (piper::is_reparse(p) || !fs::is_regular_file(p, ec))
        return false;
    const auto n = fs::file_size(p, ec);
    return !ec && n >= lo && n <= hi;
}

std::wstring manifest_name(std::string_view json, const std::wstring& id)
{
    const auto want = wide_to_utf8(id);
    if (!want)
        return id;
    size_t at = 0;
    for (size_t n = 0; n < kMaxPresets * 2 && (at = json.find("\"id\"", at)) != std::string_view::npos; ++n, ++at) {
        if (piper::string_after_key(json, "id", at, at + 8) != *want)
            continue;
        const auto name = piper::string_after_key(json, "name", at, at + 512);
        if (name.empty() || name.size() > kMaxNameBytes || name.find('\\') != std::string_view::npos)
            break;
        return utf8_to_wide(name).value_or(id);
    }
    return id;
}

}

std::optional<Heading> split_heading(std::string_view s)
{
    const std::string_view text = trim(s);
    const size_t limit = std::min(text.size(), kMaxHeadBytes);
    size_t words = 0;
    bool in_word = false;
    size_t i = 0;
    for (; i < limit; i += u8::decode(text, i).len) {
        const char32_t cp = u8::decode(text, i).cp;
        if (cp == ':')
            break;
        if (ends_sentence(cp))
            return std::nullopt;
        const bool space = u8::is_space(cp);
        words += (!space && !in_word) ? 1 : 0;
        in_word = !space;
    }
    if (i >= limit || words == 0 || words > kMaxHeadWords)
        return std::nullopt;
    const std::string_view rest = text.substr(i + 1);
    if (!rest.empty() && !u8::is_space(u8::decode(rest, 0).cp))
        return std::nullopt;
    const std::string_view tail = trim(rest);
    if (tail.empty())
        return std::nullopt;
    return Heading{std::string(trim(text.substr(0, i))) + ".", std::string(tail)};
}

std::string batch_line(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    bool gap = false;
    for (size_t i = 0; i < s.size();) {
        const auto c = u8::decode(s, i);
        if (is_separator(c.cp)) {
            gap = !out.empty();
        } else if (!is_ignorable(c.cp)) {
            if (gap)
                out.push_back(' ');
            gap = false;
            out.append(s.substr(i, c.len));
        }
        i += c.len;
    }
    return out;
}

std::string batch_file(const std::vector<std::string>& lines)
{
    std::string out;
    for (const std::string& l : lines) {
        out += l;
        out.push_back('\n');
    }
    return out;
}

std::optional<WavInfo> parse_wav(std::string_view b)
{
    if (b.size() < kWavHeaderBytes || b.size() > kMaxWavBytes || b.substr(0, 4) != "RIFF" || b.substr(8, 4) != "WAVE")
        return std::nullopt;
    WavInfo w;
    bool have_format = false;
    size_t pos = 12;
    for (size_t n = 0; n < kMaxChunks && pos + 8 <= b.size(); ++n) {
        const std::string_view id = b.substr(pos, 4);
        const size_t size = rd32(b, pos + 4);
        const size_t body = pos + 8;
        if (id == "fmt ") {
            have_format = read_format(b, body, size, w);
            if (!have_format)
                return std::nullopt;
        } else if (id == "data") {
            if (!have_format)
                return std::nullopt;
            const size_t block = size_t{w.channels} * 2;
            w.data_off = body;
            w.data_len = std::min(size, b.size() - body);
            w.data_len -= w.data_len % block;
            return w;
        }
        pos = body + size + (size & 1);
    }
    return std::nullopt;
}

std::optional<std::string> join_wavs(const std::vector<WavPart>& parts)
{
    std::vector<WavInfo> infos;
    size_t total = 0;
    for (const WavPart& p : parts) {
        const auto w = parse_wav(p.wav);
        if (!w || (!infos.empty() && (w->rate != infos[0].rate || w->channels != infos[0].channels)))
            return std::nullopt;
        total += w->data_len + static_cast<size_t>(w->rate) * p.silence_before_ms / 1000 * w->channels * 2;
        infos.push_back(*w);
        if (total > kMaxWavBytes)
            return std::nullopt;
    }
    if (infos.empty())
        return std::nullopt;
    std::string out = header(infos[0], total);
    out.reserve(kWavHeaderBytes + total);
    for (size_t i = 0; i < parts.size(); ++i) {
        const size_t frames = static_cast<size_t>(infos[i].rate) * parts[i].silence_before_ms / 1000;
        out.append(frames * infos[i].channels * 2, '\0');
        out.append(parts[i].wav.substr(infos[i].data_off, infos[i].data_len));
    }
    return out;
}

std::optional<Preset> find_preset(const fs::path& voices_dir, const std::wstring& id)
{
    if (!plain_id(id))
        return std::nullopt;
    const fs::path dir = voices_dir / id;
    std::error_code ec;
    Preset p{id, id, dir / L"speaker.emb.txt", dir / L"ref_codes.txt"};
    if (piper::is_reparse(dir) || !fs::is_directory(dir, ec) || !sized_file(p.emb, kEmbMin, kEmbMax)
        || !sized_file(p.codes, kCodesMin, kCodesMax))
        return std::nullopt;
    return p;
}

std::vector<Preset> discover_presets(const fs::path& voices_dir)
{
    std::vector<Preset> out;
    std::error_code ec;
    std::optional<std::string> manifest;
    size_t seen = 0;
    for (fs::directory_iterator it(voices_dir, ec), end; !ec && it != end && out.size() < kMaxPresets && seen++ < kMaxPresetDirs;
         it.increment(ec)) {
        auto p = find_preset(voices_dir, it->path().filename().wstring());
        if (!p)
            continue;
        if (!manifest)
            manifest = read_head(voices_dir / L"manifest.json");
        p->name = manifest_name(*manifest, p->id);
        out.push_back(std::move(*p));
    }
    std::ranges::sort(out, {}, &Preset::name);
    return out;
}

}
