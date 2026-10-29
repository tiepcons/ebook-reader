#include "tts/vieneu/VieNeuVerify.h"

#include "VieNeuPins.h"
#include "base/Text.h"

#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <sstream>

namespace eb::vieneu {

namespace {

constexpr uintmax_t kMaxHashBytes = uintmax_t{2048} * 1024 * 1024;
constexpr uintmax_t kSmallFileMax = size_t{2} * 1024 * 1024;
constexpr size_t kCacheMax = size_t{256} * 1024;
constexpr size_t kChunk = size_t{1024} * 1024;

class Sha256 {
public:
    Sha256() noexcept
    {
        ok_ = BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0
            && BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0) >= 0;
    }
    ~Sha256()
    {
        if (hash_ != nullptr)
            BCryptDestroyHash(hash_);
        if (alg_ != nullptr)
            BCryptCloseAlgorithmProvider(alg_, 0);
    }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    bool add(const char* data, size_t n) noexcept
    {
        return ok_ && BCryptHashData(hash_, reinterpret_cast<PUCHAR>(const_cast<char*>(data)), static_cast<ULONG>(n), 0) >= 0; // NOLINT(cppcoreguidelines-pro-type-const-cast,cppcoreguidelines-pro-type-reinterpret-cast)
    }
    std::string finish() noexcept
    {
        std::array<UCHAR, 32> d{};
        if (!ok_ || BCryptFinishHash(hash_, d.data(), static_cast<ULONG>(d.size()), 0) < 0)
            return {};
        constexpr char kDigits[] = "0123456789abcdef";
        try {
            std::string s;
            for (const UCHAR b : d) {
                s.push_back(kDigits[b >> 4]);
                s.push_back(kDigits[b & 15]);
            }
            return s;
        } catch (...) {
            return {};
        }
    }

private:
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    bool ok_ = false;
};

// sha256 by "path|size|mtime"; remembers what this run used and rewrites the file only when it learned something.
class HashCache {
public:
    explicit HashCache(fs::path file) : file_(std::move(file))
    {
        std::ifstream f(file_, std::ios::binary);
        std::string text(kCacheMax, '\0');
        f.read(text.data(), static_cast<std::streamsize>(text.size()));
        text.resize(static_cast<size_t>(f.gcount()));
        std::istringstream in(text);
        for (std::string line; std::getline(in, line);) {
            if (line.size() > 65 && line[64] == ' ')
                old_[line.substr(65)] = line.substr(0, 64);
        }
    }

    std::string sha(const fs::path& p, uintmax_t max_bytes)
    {
        std::error_code ec;
        const auto size = fs::file_size(p, ec);
        const auto mtime = fs::last_write_time(p, ec).time_since_epoch().count();
        const auto path = wide_to_utf8(p.wstring());
        if (ec || !path)
            return {};
        const std::string key = *path + "|" + std::to_string(size) + "|" + std::to_string(mtime);
        std::string& slot = used_[key];
        if (const auto it = old_.find(key); it != old_.end()) {
            slot = it->second;
        } else {
            slot = sha256_file(p, max_bytes);
            dirty_ = true;
        }
        return slot;
    }

    void save()
    {
        if (!dirty_ && used_.size() == old_.size())
            return;
        std::string out;
        for (const auto& [key, sha] : used_) {
            if (sha.size() != 64)
                continue;
            out += sha;
            out += ' ';
            out += key;
            out += '\n';
        }
        std::ofstream(file_, std::ios::binary | std::ios::trunc) << out;
    }

private:
    fs::path file_;
    std::map<std::string, std::string> old_;
    std::map<std::string, std::string> used_;
    bool dirty_ = false;
};

std::string_view pin_for(const Pins& pins, std::string_view name) noexcept
{
    const auto it = std::ranges::find_if(pins.voice_files, [&](const Pin& p) { return p.name == name; });
    return it == pins.voice_files.end() ? std::string_view{} : it->sha256;
}

bool matches(HashCache& cache, const fs::path& file, std::string_view want, uintmax_t max_bytes)
{
    return !want.empty() && cache.sha(file, max_bytes) == want;
}

std::vector<Preset> verified_presets(const piper::Cfg& c, const Pins& pins, HashCache& cache)
{
    const fs::path dir = voices_dir(c);
    const bool names_ok = matches(cache, dir / L"manifest.json", pins.manifest, kSmallFileMax);
    std::vector<Preset> out;
    for (Preset& p : discover_presets(dir)) {
        const auto id = wide_to_utf8(p.id);
        if (!id)
            continue;
        if (!matches(cache, p.emb, pin_for(pins, *id + "/speaker.emb.txt"), kSmallFileMax)
            || !matches(cache, p.codes, pin_for(pins, *id + "/ref_codes.txt"), kSmallFileMax))
            continue;
        if (!names_ok)
            p.name = p.id;
        out.push_back(std::move(p));
    }
    return out;
}

}

const Pins& builtin_pins()
{
    static const Pins pins = [] {
        Pins p{pins::kModel, pins::kDict, pins::kManifest, {}};
        for (const auto& f : pins::kVoiceFiles)
            p.voice_files.push_back({f.name, f.sha256});
        return p;
    }();
    return pins;
}

std::string sha256_file(const fs::path& file, uintmax_t max_bytes) noexcept
{
    try {
        std::error_code ec;
        if (piper::is_reparse(file) || !fs::is_regular_file(file, ec) || fs::file_size(file, ec) > max_bytes || ec)
            return {};
        std::ifstream f(file, std::ios::binary);
        Sha256 h;
        std::vector<char> buf(kChunk);
        while (f.read(buf.data(), static_cast<std::streamsize>(buf.size())) || f.gcount() > 0) {
            if (!h.add(buf.data(), static_cast<size_t>(f.gcount())))
                return {};
        }
        return f.eof() ? h.finish() : std::string{};
    } catch (...) {
        return {};
    }
}

std::optional<Verified> verify_install(const piper::Cfg& c, const Pins& pins, const fs::path& cache_file) noexcept
{
    try {
        auto assets = find_assets(c);
        if (!assets)
            return std::nullopt;
        HashCache cache(cache_file);
        if (!matches(cache, assets->model, pins.model, kMaxHashBytes) || !matches(cache, assets->g2p_dict, pins.dict, kMaxHashBytes))
            return std::nullopt;
        Verified v{*assets, verified_presets(c, pins, cache)};
        cache.save();
        if (v.presets.empty())
            return std::nullopt;
        return v;
    } catch (...) {
        return std::nullopt;
    }
}

}
