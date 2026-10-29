#include "tts/azure/AudioCache.h"

#include "base/AtomicFile.h"
#include "base/FileIo.h"
#include "base/PathSafety.h"
#include "tts/azure/AzureHttp.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>

namespace eb {
namespace {

constexpr size_t kKeyChars = 64;
constexpr size_t kMaxEntries = 200000;
constexpr std::wstring_view kExt = L".mp3";

struct Item {
    std::filesystem::path path;
    uint64_t size{};
    uint64_t mtime{};
};

template <class Sv>
bool hex64(Sv s) noexcept
{
    return s.size() == kKeyChars && std::ranges::all_of(s, [](auto c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool valid_key(std::string_view key) noexcept
{
    return hex64(key);
}

// "<64 hex>.mp3.tmp<digits>", the name write_atomic gives its temporary file.
bool is_leftover_name(std::wstring_view n) noexcept
{
    constexpr std::wstring_view tail = L".mp3.tmp";
    if (n.size() <= kKeyChars + tail.size() || !hex64(n.substr(0, kKeyChars)) || n.substr(kKeyChars, tail.size()) != tail)
        return false;
    return std::ranges::all_of(n.substr(kKeyChars + tail.size()), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
}

// Attributes of the directory entry itself: a symlink or junction is reported as a reparse point and never followed.
bool plain_file_info(const std::filesystem::path& p, uint64_t& size, uint64_t& mtime) noexcept
{
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d))
        return false;
    if ((d.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0)
        return false;
    size = (uint64_t{d.nFileSizeHigh} << 32) | d.nFileSizeLow;
    mtime = (uint64_t{d.ftLastWriteTime.dwHighDateTime} << 32) | d.ftLastWriteTime.dwLowDateTime;
    return true;
}

std::vector<Item> scan(const std::filesystem::path& dir, bool remove_leftovers)
{
    std::vector<Item> items;
    if (!is_plain_dir_chain(dir))
        return items;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end && items.size() < kMaxEntries;
         it.increment(ec)) {
        const std::wstring name = it->path().filename().wstring();
        Item item{it->path(), 0, 0};
        if (name.size() == kKeyChars + kExt.size() && name.ends_with(kExt) && hex64(std::wstring_view(name).substr(0, kKeyChars))) {
            if (plain_file_info(item.path, item.size, item.mtime))
                items.push_back(std::move(item));
        } else if (remove_leftovers && is_leftover_name(name)) {
            uint64_t size = 0;
            uint64_t mtime = 0;
            std::error_code e2;
            if (plain_file_info(item.path, size, mtime))
                std::filesystem::remove(item.path, e2);
        }
    }
    return items;
}

}

AudioCache::AudioCache(std::filesystem::path dir, uint64_t cap_bytes) noexcept : dir_(std::move(dir)), cap_(cap_bytes)
{
    try {
        std::error_code ec;
        std::filesystem::create_directories(dir_, ec);
        const std::scoped_lock lk(m_);
        total_ = std::ranges::fold_left(scan(dir_, true), uint64_t{0}, [](uint64_t a, const Item& i) { return a + i.size; });
        enforce_locked();
    } catch (...) { // NOLINT(bugprone-empty-catch): a cache that cannot start simply stays empty
    }
}

std::string AudioCache::key_for(std::string_view voice, int rate_pct, std::string_view text) noexcept
{
    try {
        std::string in;
        in.reserve(voice.size() + text.size() + 16);
        in.append(voice).append("|").append(std::to_string(rate_pct)).append("|").append(text);
        std::array<UCHAR, 32> digest{};
        if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(in.data()), static_cast<ULONG>(in.size()),
                digest.data(), static_cast<ULONG>(digest.size()))
            < 0)
            return {};
        static constexpr char kDigits[] = "0123456789abcdef";
        std::string hex;
        hex.reserve(kKeyChars);
        for (const UCHAR b : digest) {
            hex.push_back(kDigits[b >> 4]);
            hex.push_back(kDigits[b & 15]);
        }
        return hex;
    } catch (...) {
        return {};
    }
}

std::filesystem::path AudioCache::path_for(std::string_view key) const
{
    return dir_ / (std::wstring(key.begin(), key.end()) + std::wstring(kExt));
}

std::optional<std::vector<std::byte>> AudioCache::get(std::string_view key) noexcept
{
    try {
        if (!valid_key(key))
            return std::nullopt;
        const std::scoped_lock lk(m_);
        const auto path = path_for(key);
        uint64_t on_disk = 0;
        uint64_t mtime = 0;
        if (!is_plain_dir_chain(dir_) || !plain_file_info(path, on_disk, mtime))
            return std::nullopt;
        const auto bytes = read_all(path, kAzureMaxResponse);
        if (!bytes || bytes->empty())
            return std::nullopt;
        const auto* first = reinterpret_cast<const std::byte*>(bytes->data());
        std::vector<std::byte> data(first, first + bytes->size());
        std::error_code ec;
        std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(), ec);
        return data;
    } catch (...) {
        return std::nullopt;
    }
}

bool AudioCache::put(std::string_view key, std::span<const std::byte> data) noexcept
{
    try {
        if (!valid_key(key) || data.empty() || data.size() > kAzureMaxResponse || !is_plain_dir_chain(dir_))
            return false;
        const std::scoped_lock lk(m_);
        const auto path = path_for(key);
        uint64_t old = 0;
        uint64_t mtime = 0;
        if (!plain_file_info(path, old, mtime))
            old = 0;
        if (!write_atomic(path, data))
            return false;
        total_ = total_ - std::min<uint64_t>(total_, old) + data.size();
        if (total_ > cap_)
            enforce_locked();
        return true;
    } catch (...) {
        return false;
    }
}

uint64_t AudioCache::total_bytes() noexcept
{
    const std::scoped_lock lk(m_);
    return total_;
}

void AudioCache::enforce_locked() noexcept
{
    try {
        auto items = scan(dir_, false);
        uint64_t total = std::ranges::fold_left(items, uint64_t{0}, [](uint64_t a, const Item& i) { return a + i.size; });
        std::ranges::sort(items, [](const Item& a, const Item& b) {
            return a.mtime != b.mtime ? a.mtime < b.mtime : a.path < b.path;
        });
        for (const Item& i : items) {
            if (total <= cap_)
                break;
            std::error_code ec;
            if (std::filesystem::remove(i.path, ec) && !ec)
                total -= i.size;
        }
        total_ = total;
    } catch (...) { // NOLINT(bugprone-empty-catch): eviction is best-effort
    }
}

}
