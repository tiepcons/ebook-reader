#include "doc/TxtDecode.h"

#include "base/Text.h"

#include <windows.h>

#include <algorithm>
#include <array>

namespace eb {
namespace {

constexpr std::size_t kSniffBytes = 65536;
constexpr std::size_t kNfcChunk = 16384;
constexpr int kNormalizeRetries = 4;

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
    const char* const src = s.data();
    const int n = MultiByteToWideChar(1258, 0, src, len, nullptr, 0);
    if (n <= 0) {
        return fail(ErrorCode::Format, L"cp1258 decode failed");
    }
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    if (MultiByteToWideChar(1258, 0, src, len, out.data(), n) != n) {
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

void sanitize(std::wstring& s)
{
    std::size_t w = 0;
    const std::size_t n = s.size();
    for (std::size_t i = 0; i < n; ++i) {
        wchar_t c = s[i];
        if (c == L'\r') {
            if (i + 1 < n && s[i + 1] == L'\n') {
                continue;
            }
            c = L'\n';
        }
        if (is_high(c)) {
            if (i + 1 < n && is_low(s[i + 1])) {
                s[w++] = c;
                s[w++] = s[++i];
            }
            continue;
        }
        if (!is_low(c) && (c == L'\n' || c == L'\t' || !is_control(c))) {
            s[w++] = c;
        }
    }
    s.resize(w);
}

bool normalize_chunk(std::wstring_view in, std::wstring& out)
{
    const int len = static_cast<int>(in.size());
    const wchar_t* const src = in.data();
    int cap = NormalizeString(NormalizationC, src, len, nullptr, 0);
    for (int attempt = 0; attempt < kNormalizeRetries && cap > 0; ++attempt) {
        const std::size_t base = out.size();
        out.resize(base + static_cast<std::size_t>(cap));
        const int r = NormalizeString(NormalizationC, src, len, out.data() + base, cap);
        if (r > 0) {
            out.resize(base + static_cast<std::size_t>(r));
            return true;
        }
        out.resize(base);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            break;
        }
        cap = -r;
    }
    return false;
}

std::size_t chunk_end(std::wstring_view s, std::size_t pos) noexcept
{
    const std::size_t target = pos + kNfcChunk;
    if (target >= s.size()) {
        return s.size();
    }
    const std::size_t nl = s.substr(0, std::min(s.size(), target + kNfcChunk)).find(L'\n', target);
    if (nl != std::wstring_view::npos) {
        return nl + 1;
    }
    std::size_t cut = target;
    while (cut > pos + 1 && ((s[cut] >= 0x300 && s[cut] <= 0x36F) || is_low(s[cut]))) {
        --cut;
    }
    return cut;
}

// NFC is the identity below U+0300, so pure Latin text skips the second buffer entirely.
void to_nfc(std::wstring& s)
{
    if (std::ranges::all_of(s, [](wchar_t c) { return c < 0x300; })) {
        return;
    }
    std::wstring out;
    out.reserve(s.size());
    for (std::size_t pos = 0; pos < s.size();) {
        const std::size_t end = chunk_end(s, pos);
        const std::wstring_view chunk = std::wstring_view(s).substr(pos, end - pos);
        if (!normalize_chunk(chunk, out)) {
            out.append(chunk);
        }
        pos = end;
    }
    s.swap(out);
    std::wstring().swap(out);
}

}

Result<std::wstring> decode_text(std::span<const std::byte> raw)
{
    auto decoded = decode_raw(raw);
    if (!decoded) {
        return decoded;
    }
    sanitize(*decoded);
    to_nfc(*decoded);
    return decoded;
}

}
