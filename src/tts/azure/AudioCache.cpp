#include "tts/azure/AudioCache.h"

#include "base/AtomicFile.h"
#include "tts/azure/AzureHttp.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <fstream>

namespace eb {
namespace {

constexpr size_t kKeyChars = 64;
constexpr size_t kMaxEntries = 200000;
constexpr std::wstring_view kExt = L".mp3";

struct Item {
    std::filesystem::path path;
    uint64_t size;
    std::filesystem::file_time_type mtime;
};

bool valid_key(std::string_view key) noexcept
{
    return key.size() == kKeyChars
        && std::ranges::all_of(key, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::vector<Item> scan(const std::filesystem::path& dir, bool remove_leftovers)
{
    std::vector<Item> items;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end && items.size() < kMaxEntries;
         it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2))
            continue;
        const auto ext = it->path().extension().wstring();
        if (ext == kExt) {
            const auto size = it->file_size(e2);
            const auto mtime = it->last_write_time(e2);
            if (!e2)
                items.push_back({it->path(), size, mtime});
        } else if (remove_leftovers && ext.starts_with(L".tmp")) {
            std::filesystem::remove(it->path(), e2);
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
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f)
            return std::nullopt;
        const auto size = static_cast<size_t>(f.tellg());
        if (size == 0 || size > kAzureMaxResponse)
            return std::nullopt;
        std::vector<std::byte> data(size);
        f.seekg(0);
        if (!f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size)))
            return std::nullopt;
        f.close();
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
        if (!valid_key(key) || data.empty() || data.size() > kAzureMaxResponse)
            return false;
        const std::scoped_lock lk(m_);
        const auto path = path_for(key);
        std::error_code ec;
        const auto old = std::filesystem::file_size(path, ec);
        if (!write_atomic(path, data))
            return false;
        total_ = total_ - std::min<uint64_t>(total_, ec ? 0 : old) + data.size();
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
