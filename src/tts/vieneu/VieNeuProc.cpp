#include "tts/vieneu/VieNeuProc.h"

#include "tts/piper/PiperTts.h"

#include <algorithm>
#include <fstream>
#include <thread>

namespace eb::vieneu {

namespace {

constexpr size_t kMaxDirEntries = 256;
constexpr uintmax_t kModelMin = size_t{32} * 1024 * 1024;
constexpr uintmax_t kModelMax = size_t{2048} * 1024 * 1024;
constexpr uintmax_t kDictMin = size_t{4} * 1024 * 1024;
constexpr uintmax_t kDictMax = size_t{512} * 1024 * 1024;
constexpr uintmax_t kLibMin = size_t{256} * 1024;
constexpr uintmax_t kLibMax = size_t{64} * 1024 * 1024;

bool plain_file(const fs::path& p, uintmax_t lo, uintmax_t hi) noexcept
{
    std::error_code ec;
    if (piper::is_reparse(p) || !fs::is_regular_file(p, ec))
        return false;
    const auto n = fs::file_size(p, ec);
    return !ec && n >= lo && n <= hi;
}

bool has_gguf_magic(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    char magic[4] = {};
    f.read(magic, sizeof magic);
    return f.gcount() == 4 && std::string_view(magic, 4) == "GGUF";
}

bool is_model_name(const std::wstring& name)
{
    std::wstring lower = name;
    std::ranges::transform(lower, lower.begin(), [](wchar_t c) { return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + 32) : c; });
    return lower.starts_with(L"vieneu") && lower.ends_with(L".gguf");
}

fs::path find_model(const fs::path& dir)
{
    std::error_code ec;
    size_t seen = 0;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end && seen++ < kMaxDirEntries; it.increment(ec)) {
        const fs::path& p = it->path();
        if (is_model_name(p.filename().wstring()) && plain_file(p, kModelMin, kModelMax) && has_gguf_magic(p))
            return p;
    }
    return {};
}

std::wstring option(const wchar_t* key, const fs::path& value)
{
    return L" " + std::wstring(key) + L" " + piper::quote_arg(value.wstring());
}

std::wstring key_value(const wchar_t* flag, const wchar_t* key, const fs::path& value)
{
    return L" " + std::wstring(flag) + L" " + piper::quote_arg(std::wstring(key) + L"=" + value.wstring());
}

}

piper::Cfg make_cfg(const fs::path& libs_dir, const fs::path& data_dir, const fs::path& temp_dir)
{
    std::error_code ec;
    piper::Cfg c{fs::absolute(libs_dir, ec), fs::absolute(data_dir, ec), fs::absolute(temp_dir, ec)};
    c.exe_name = L"audiocpp_cli.exe";
    return c;
}

std::optional<Assets> find_assets(const piper::Cfg& c)
{
    try {
        Assets a{find_model(c.voices), c.voices / L"sea_g2p.bin", c.piper / L"sea_g2p_rs.dll"};
        if (a.model.empty() || !piper::safe_exe(c) || !plain_file(a.g2p_dict, kDictMin, kDictMax)
            || !plain_file(a.g2p_lib, kLibMin, kLibMax))
            return std::nullopt;
        return a;
    } catch (...) {
        return std::nullopt;
    }
}

unsigned child_threads() noexcept
{
    return std::clamp(std::thread::hardware_concurrency() / 2, 1U, 4U);
}

std::wstring command_line(const piper::Cfg& c, const Assets& a, const Preset& voice, const fs::path& batch_txt,
    const fs::path& out_dir, unsigned threads)
{
    std::wstring cmd = piper::quote_arg(c.exe().wstring()) + L" --task tts --family vieneu_v3_turbo --backend cpu --log";
    cmd += option(L"--model", a.model);
    cmd += L" --threads " + std::to_wstring(threads);
    cmd += option(L"--batch-text-file", batch_txt);
    cmd += option(L"--out-dir", out_dir);
    cmd += key_value(L"--session-option", L"g2p_dict_file", a.g2p_dict);
    cmd += key_value(L"--session-option", L"g2p_library", a.g2p_lib);
    cmd += key_value(L"--request-option", L"speaker_embedding_file", voice.emb);
    cmd += key_value(L"--request-option", L"reference_codes_file", voice.codes);
    return cmd;
}

}
