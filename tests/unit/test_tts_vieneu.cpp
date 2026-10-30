#include "tts/proc/ProcShared.h"
#include "tts/vieneu/VieNeuEngine.h"
#include "tts/vieneu/VieNeuProc.h"
#include "tts/vieneu/VieNeuTts.h"
#include "tts/vieneu/VieNeuVerify.h"

#include "base/Text.h"
#include "tts_sink.h"
#include "test_perf.h"

#include <doctest/doctest.h>

#include <tlhelp32.h>

#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace eb_test;

struct TmpDir {
    fs::path dir;
    explicit TmpDir(const wchar_t* name) : dir(fs::temp_directory_path() / name)
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
    }
    ~TmpDir()
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    TmpDir(const TmpDir&) = delete;
    TmpDir& operator=(const TmpDir&) = delete;
};

void write_file(const fs::path& p, const std::string& content, uintmax_t size = 0)
{
    fs::create_directories(p.parent_path());
    {
        std::ofstream f(p, std::ios::binary);
        f << content;
    }
    if (size > content.size()) {
        std::error_code ec;
        fs::resize_file(p, size, ec);
    }
}

constexpr uintmax_t kMiB = uintmax_t{1024} * 1024;

void add_preset(const fs::path& voices, const std::string& id)
{
    write_file(voices / id / "speaker.emb.txt", std::string(2200, '1'));
    write_file(voices / id / "ref_codes.txt", std::string(3000, '2'));
}

// libs\vieneu and data\vieneu with stand-in files of believable sizes.
struct Fixture {
    TmpDir t{L"eb-vieneu-fixture"};
    fs::path libs = t.dir / L"libs";
    fs::path data = t.dir / L"data";
    eb::vieneu::Pins pins;

    Fixture()
    {
        write_file(libs / L"audiocpp_cli.exe", "MZ", kMiB);
        write_file(libs / L"sea_g2p_rs.dll", "MZ", kMiB);
        write_file(data / L"vieneu-v3-turbo-q8_0.gguf", "GGUF", 40 * kMiB);
        write_file(data / L"sea_g2p.bin", "x", 8 * kMiB);
        add_preset(data / L"voices", "ngoc_huyen");
        add_preset(data / L"voices", "minh_quan_pro");
        pin();
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    // Pins that match the stand-in files as they are now.
    void pin()
    {
        constexpr uintmax_t kMax = uintmax_t{1} << 31;
        pins = {};
        text_.clear();
        const auto c = eb::vieneu::make_cfg(libs, data, t.dir / L"tmp");
        const auto a = eb::vieneu::find_assets(c);
        pins.model = keep(eb::vieneu::sha256_file(a->model, kMax));
        pins.dict = keep(eb::vieneu::sha256_file(a->g2p_dict, kMax));
        for (const auto& p : eb::vieneu::discover_presets(eb::vieneu::voices_dir(c))) {
            const std::string id = eb::wide_to_utf8(p.id).value_or("");
            pins.voice_files.push_back({keep(id + "/speaker.emb.txt"), keep(eb::vieneu::sha256_file(p.emb, kMax))});
            pins.voice_files.push_back({keep(id + "/ref_codes.txt"), keep(eb::vieneu::sha256_file(p.codes, kMax))});
        }
    }

private:
    std::string_view keep(const std::string& s)
    {
        text_.push_back(s);
        return text_.back();
    }
    std::deque<std::string> text_; // owns what the pins point to
};

bool wait_ready(const eb::TtsEngine& tts, int timeout_ms = 60000)
{
    const auto end = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < end) {
        if (tts.ready())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

}

TEST_CASE("vieneu assets: complete installs are found")
{
    const Fixture f;
    const auto c = eb::vieneu::make_cfg(f.libs, f.data, f.t.dir / L"tmp");
    const auto a = eb::vieneu::find_assets(c);
    REQUIRE(a.has_value());
    CHECK(a->model.filename() == L"vieneu-v3-turbo-q8_0.gguf");
    CHECK(a->g2p_dict.filename() == L"sea_g2p.bin");
    CHECK(a->g2p_lib.filename() == L"sea_g2p_rs.dll");
}

TEST_CASE("vieneu assets: any missing or implausible file hides the engine")
{
    const Fixture f;
    const auto c = eb::vieneu::make_cfg(f.libs, f.data, f.t.dir / L"tmp");
    std::error_code ec;

    write_file(f.data / L"vieneu-v3-turbo-q8_0.gguf", "NOPE", 40 * kMiB);
    CHECK_FALSE(eb::vieneu::find_assets(c));
    write_file(f.data / L"vieneu-v3-turbo-q8_0.gguf", "GGUF", 1 * kMiB);
    CHECK_FALSE(eb::vieneu::find_assets(c));
    write_file(f.data / L"vieneu-v3-turbo-q8_0.gguf", "GGUF", 40 * kMiB);
    REQUIRE(eb::vieneu::find_assets(c));

    fs::rename(f.data / L"vieneu-v3-turbo-q8_0.gguf", f.data / L"other.gguf", ec);
    CHECK_FALSE(eb::vieneu::find_assets(c));
    fs::rename(f.data / L"other.gguf", f.data / L"VieNeu-q4.GGUF", ec);
    CHECK(eb::vieneu::find_assets(c));

    write_file(f.data / L"sea_g2p.bin", "x", 100);
    CHECK_FALSE(eb::vieneu::find_assets(c));
    write_file(f.data / L"sea_g2p.bin", "x", 8 * kMiB);
    fs::remove(f.libs / L"sea_g2p_rs.dll", ec);
    CHECK_FALSE(eb::vieneu::find_assets(c));
    write_file(f.libs / L"sea_g2p_rs.dll", "MZ", kMiB);
    fs::remove(f.libs / L"audiocpp_cli.exe", ec);
    CHECK_FALSE(eb::vieneu::find_assets(c));
    CHECK_FALSE(eb::vieneu::find_assets(eb::vieneu::make_cfg(f.t.dir / L"none", f.data, f.t.dir / L"tmp")));
}

TEST_CASE("vieneu command line: batch, output dir, voice files and g2p options are quoted")
{
    const Fixture f;
    const auto c = eb::vieneu::make_cfg(f.libs, f.data, f.t.dir / L"tmp dir");
    const auto a = eb::vieneu::find_assets(c);
    REQUIRE(a.has_value());
    const auto v = eb::vieneu::find_preset(eb::vieneu::voices_dir(c), L"ngoc_huyen");
    REQUIRE(v.has_value());
    const std::wstring cmd = eb::vieneu::command_line(c, *a, *v, c.temp / L"b 1" / L"in.txt", c.temp / L"b 1", 3);
    CHECK(cmd.find(L"--family vieneu_v3_turbo") != std::wstring::npos);
    CHECK(cmd.find(L"--backend cpu") != std::wstring::npos);
    CHECK(cmd.find(L"--threads 3") != std::wstring::npos);
    CHECK(cmd.find(L"--log") != std::wstring::npos);
    CHECK(cmd.find(L"--batch-text-file \"" + (c.temp / L"b 1" / L"in.txt").wstring() + L"\"") != std::wstring::npos);
    CHECK(cmd.find(L"--out-dir \"" + (c.temp / L"b 1").wstring() + L"\"") != std::wstring::npos);
    // audio.cpp silently ignores unprefixed session options (the text is then read as phonemes).
    CHECK(cmd.find(L"vieneu_v3_turbo.g2p_dict_file=") != std::wstring::npos);
    CHECK(cmd.find(L"vieneu_v3_turbo.g2p_library=") != std::wstring::npos);
    CHECK(cmd.find(L" g2p_dict_file=") == std::wstring::npos);
    CHECK(cmd.find(L"\"g2p_dict_file=") == std::wstring::npos);
    CHECK(cmd.find(L"speaker_embedding_file=") != std::wstring::npos);
    CHECK(cmd.find(L"reference_codes_file=") != std::wstring::npos);
    CHECK(cmd.find(L"--server") == std::wstring::npos);
    CHECK(cmd.find(L"--port") == std::wstring::npos);
    const unsigned t = eb::vieneu::child_threads();
    CHECK(t >= 1);
    CHECK(t <= 4);
}

TEST_CASE("vieneu voices: every pinned preset is offered for Vietnamese and English")
{
    const Fixture f;
    eb::VieNeuTts tts(f.libs, f.data, f.t.dir / L"tmp", f.pins);
    REQUIRE(wait_ready(tts));
    const auto list = tts.voices();
    REQUIRE(list.size() == 4);
    size_t vi = 0;
    size_t en = 0;
    for (const auto& v : list) {
        vi += v.lang == L"vi-VN" ? 1 : 0;
        en += v.lang == L"en-US" ? 1 : 0;
        CHECK((v.id == L"ngoc_huyen" || v.id == L"minh_quan_pro"));
    }
    CHECK(vi == 2);
    CHECK(en == 2);
}

TEST_CASE("vieneu verify: files that differ from their pins keep the engine hidden")
{
    const Fixture f;
    const auto c = eb::vieneu::make_cfg(f.libs, f.data, f.t.dir / L"tmp");
    const fs::path cache = eb::vieneu::cache_file_of(c);
    REQUIRE(eb::vieneu::verify_install(c, f.pins, cache).has_value());

    eb::vieneu::Pins bad = f.pins;
    bad.model = "0000000000000000000000000000000000000000000000000000000000000000";
    CHECK_FALSE(eb::vieneu::verify_install(c, bad, cache));
    bad = f.pins;
    bad.dict = {};
    CHECK_FALSE(eb::vieneu::verify_install(c, bad, cache));
    bad = f.pins;
    bad.voice_files.clear();
    CHECK_FALSE(eb::vieneu::verify_install(c, bad, cache));

    eb::VieNeuTts tts(f.libs, f.data, f.t.dir / L"tmp", bad);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK_FALSE(tts.ready());
    CHECK(tts.voices().empty());
}

TEST_CASE("vieneu verify: unknown and edited voice files are ignored")
{
    const Fixture f;
    const auto c = eb::vieneu::make_cfg(f.libs, f.data, f.t.dir / L"tmp");
    const fs::path cache = eb::vieneu::cache_file_of(c);
    add_preset(f.data / L"voices", "intruder");
    auto v = eb::vieneu::verify_install(c, f.pins, cache);
    REQUIRE(v.has_value());
    CHECK(v->presets.size() == 2);

    write_file(f.data / L"voices" / "ngoc_huyen" / "ref_codes.txt", std::string(3000, '9'));
    v = eb::vieneu::verify_install(c, f.pins, cache);
    REQUIRE(v.has_value());
    REQUIRE(v->presets.size() == 1);
    CHECK(v->presets[0].id == L"minh_quan_pro");
    CHECK(v->presets[0].name == L"minh_quan_pro");
}

TEST_CASE("vieneu verify: display names come from the manifest only when it is pinned")
{
    const Fixture f;
    const auto c = eb::vieneu::make_cfg(f.libs, f.data, f.t.dir / L"tmp");
    write_file(f.data / L"voices" / "manifest.json", "{\"voices\":[{\"id\":\"ngoc_huyen\",\"name\":\"Ng Huyen\"}]}");
    const fs::path cache = eb::vieneu::cache_file_of(c);
    auto name_of = [&](const eb::vieneu::Pins& p) {
        const auto v = eb::vieneu::verify_install(c, p, cache);
        for (const auto& pr : v->presets)
            if (pr.id == L"ngoc_huyen")
                return pr.name;
        return std::wstring{};
    };
    CHECK(name_of(f.pins) == L"ngoc_huyen");
    eb::vieneu::Pins pinned = f.pins;
    const std::string sha = eb::vieneu::sha256_file(f.data / L"voices" / "manifest.json", 1 << 20);
    pinned.manifest = sha;
    CHECK(name_of(pinned) == L"Ng Huyen");
}

TEST_CASE("vieneu verify: cached hashes are keyed by size and modification time")
{
    const Fixture f;
    const auto c = eb::vieneu::make_cfg(f.libs, f.data, f.t.dir / L"tmp");
    const fs::path cache = eb::vieneu::cache_file_of(c);
    REQUIRE(eb::vieneu::verify_install(c, f.pins, cache).has_value());
    CHECK(fs::exists(cache));
    REQUIRE(eb::vieneu::verify_install(c, f.pins, cache).has_value());
    const fs::path voice = f.data / L"voices" / "minh_quan_pro" / "speaker.emb.txt";
    const auto before = fs::last_write_time(voice);
    write_file(voice, std::string(2200, '7'));
    fs::last_write_time(voice, before + std::chrono::seconds(5));
    const auto v = eb::vieneu::verify_install(c, f.pins, cache);
    REQUIRE(v.has_value());
    CHECK(v->presets.size() == 1);
}

TEST_CASE("vieneu sha256: known vector, size limit and missing files")
{
    const TmpDir t(L"eb-vieneu-sha");
    write_file(t.dir / L"abc.txt", "abc");
    CHECK(eb::vieneu::sha256_file(t.dir / L"abc.txt", 1000) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(eb::vieneu::sha256_file(t.dir / L"abc.txt", 2).empty());
    CHECK(eb::vieneu::sha256_file(t.dir / L"none.txt", 1000).empty());
    write_file(t.dir / L"empty.txt", "");
    CHECK(eb::vieneu::sha256_file(t.dir / L"empty.txt", 1000) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    write_file(t.dir / L"big.bin", "x", 3 * kMiB + 17);
    CHECK(eb::vieneu::sha256_file(t.dir / L"big.bin", 8 * kMiB).size() == 64);
}

TEST_CASE("vieneu engine without a runtime fails the utterance instead of hanging")
{
    const Fixture f;
    Sink sink;
    eb::VieNeuTts tts(f.libs, f.data, f.t.dir / L"tmp", f.pins);
    tts.set_sink(sink.hwnd, kMsg);
    REQUIRE(wait_ready(tts));
    REQUIRE(tts.speak(1, "Xin ch\xC3\xA0o.", L"ngoc_huyen", 1.0, 0.0));
    CHECK_FALSE(sink.wait_for(1, eb::TtsEvent::Finished, 20000));
    REQUIRE_FALSE(sink.got.empty());
    CHECK(sink.got.back().ev == eb::TtsEvent::Error);
    CHECK(sink.got.back().value == eb::kTtsSynthFailed);
}

TEST_CASE("vieneu engine rejects unknown voices and text that is only control characters")
{
    const Fixture f;
    Sink sink;
    eb::VieNeuTts tts(f.libs, f.data, f.t.dir / L"tmp", f.pins);
    tts.set_sink(sink.hwnd, kMsg);
    REQUIRE(wait_ready(tts));
    REQUIRE(tts.speak(1, "hello", L"..\\evil", 1.0, 0.0));
    CHECK_FALSE(sink.wait_for(1, eb::TtsEvent::Finished, 20000));
    REQUIRE_FALSE(sink.got.empty());
    CHECK(sink.got.back().value == eb::kTtsBadVoice);
    sink.got.clear();
    REQUIRE(tts.speak(2, std::string("\x01\x02"), L"ngoc_huyen", 1.0, 0.0));
    CHECK_FALSE(sink.wait_for(2, eb::TtsEvent::Finished, 5000));
    REQUIRE_FALSE(sink.got.empty());
    CHECK(sink.got.back().ev == eb::TtsEvent::Error);
    sink.got.clear();
    CHECK_FALSE(tts.speak(3, std::string(eb::kTtsMaxUtf8 + 1, 'a'), L"ngoc_huyen", 1.0, 0.0));
    sink.pump_ms(50);
    REQUIRE_FALSE(sink.got.empty());
    CHECK(sink.got.back().value == eb::kTtsTextTooLarge);
}

namespace {

// build/spike/vieneu: bin = audio.cpp CPU release + sea_g2p_rs.dll, model\*.gguf + voices, g2p\sea_g2p.bin.
struct Spike {
    fs::path root = EB_VIENEU_SPIKE;
    fs::path libs = root / L"bin";
    fs::path data = root.parent_path() / L"vieneu-test-data";
    fs::path temp = fs::temp_directory_path() / L"eb-vieneu-test";

    [[nodiscard]] bool present() const
    {
        std::error_code ec;
        return fs::exists(libs / L"audiocpp_cli.exe", ec) && fs::exists(libs / L"sea_g2p_rs.dll", ec)
            && fs::exists(root / L"model" / L"vieneu-v3-turbo-q8_0.gguf", ec) && fs::exists(root / L"g2p" / L"sea_g2p.bin", ec)
            && fs::exists(root / L"model" / L"voices" / L"ngoc_huyen" / L"ref_codes.txt", ec);
    }
    // The data folder the engine expects, built from the spike files (hard links, copies where links are impossible).
    [[nodiscard]] bool stage() const
    {
        std::error_code ec;
        const std::pair<fs::path, fs::path> big[] = {{root / L"model" / L"vieneu-v3-turbo-q8_0.gguf", data / L"vieneu-v3-turbo-q8_0.gguf"},
            {root / L"g2p" / L"sea_g2p.bin", data / L"sea_g2p.bin"}};
        fs::create_directories(data, ec);
        for (const auto& [from, to] : big) {
            if (fs::exists(to, ec))
                continue;
            fs::create_hard_link(from, to, ec);
            if (ec)
                fs::copy_file(from, to, ec);
        }
        fs::copy(root / L"model" / L"voices", data / L"voices", fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
        return !ec;
    }
};

#define REQUIRE_VIENEU(s)                                                                                             \
    const Spike s;                                                                                                    \
    if (!s.present() || !s.stage()) {                                                                                 \
        MESSAGE("VieNeu spike assets (build/spike/vieneu) missing, skipped");                                         \
        return;                                                                                                       \
    }

std::vector<DWORD> cli_children()
{
    std::vector<DWORD> out;
    const auto snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return out;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (pe.th32ParentProcessID == GetCurrentProcessId() && std::wstring_view(pe.szExeFile) == L"audiocpp_cli.exe")
            out.push_back(pe.th32ProcessID);
    CloseHandle(snap);
    return out;
}

bool wait_children(bool present, int timeout_ms)
{
    const auto end = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < end) {
        if (cli_children().empty() != present)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

size_t largest_wav_bytes(const fs::path& dir)
{
    size_t best = 0;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() == L".wav") {
            std::error_code sec;
            best = std::max<size_t>(best, fs::file_size(it->path(), sec));
        }
    }
    return best;
}

const std::string kViText = "Tr\xE1\xBB\x9Di \xC4\x91\xC3\xA3 v\xE1\xBB\x81 chi\xE1\xBB\x81u, gi\xC3\xB3 th\xE1\xBB\x95i qua nh\xE1\xBB\xAFng m\xC3\xA1i nh\xC3\xA0.";
const std::wstring kVoice = L"ngoc_huyen";

}

TEST_CASE("vieneu real engine speaks a Vietnamese sentence: Started then Finished with audio in between")
{
    REQUIRE_VIENEU(s);
    Sink sink;
    eb::VieNeuTts tts(s.libs, s.data, s.temp);
    tts.set_sink(sink.hwnd, kMsg);
    REQUIRE(wait_ready(tts));
    size_t wav_bytes = 0;
    g_on_event = [&](const Got& g) {
        if (g.ev == eb::TtsEvent::Started)
            wav_bytes = largest_wav_bytes(eb::vieneu::play_dir(eb::vieneu::make_cfg(s.libs, s.data, s.temp)));
    };
    REQUIRE(tts.speak(1, kViText, kVoice, 1.0, 0.0));
    REQUIRE(sink.wait_for(1, eb::TtsEvent::Finished, 90000));
    std::vector<eb::TtsEvent> seq;
    seq.reserve(sink.got.size());
    for (const auto& g : sink.got)
        seq.push_back(g.ev);
    CHECK(seq == std::vector<eb::TtsEvent>{eb::TtsEvent::Started, eb::TtsEvent::Finished});
    CHECK(wav_bytes > 100000);
    const auto played = std::chrono::duration_cast<std::chrono::milliseconds>(sink.got.back().at - sink.got.front().at).count();
    CHECK(played > 1500);
}

TEST_CASE("vieneu real engine batches announced sentences and starts the next one at once")
{
    REQUIRE_VIENEU(s);
    Sink sink;
    eb::VieNeuTts tts(s.libs, s.data, s.temp);
    tts.set_sink(sink.hwnd, kMsg);
    REQUIRE(wait_ready(tts));
    const std::string second = "The old lighthouse keeper climbed the stairs slowly.";
    const std::string third = "Lam Phan walked into the city.";
    Clock::time_point finished_first{};
    g_on_event = [&](const Got& g) {
        if (g.id == 1 && g.ev == eb::TtsEvent::Finished) {
            finished_first = g.at;
            tts.speak(2, second, kVoice, 1.0, 0.0);
            tts.prepare(3, third, kVoice, 1.0);
        }
        if (g.id == 2 && g.ev == eb::TtsEvent::Finished)
            tts.speak(3, third, kVoice, 1.0, 0.0);
    };
    REQUIRE(tts.speak(1, kViText, kVoice, 1.0, 0.0));
    tts.prepare(2, second, kVoice, 1.0);
    tts.prepare(3, third, kVoice, 1.0);
    REQUIRE(sink.wait_for(3, eb::TtsEvent::Finished, 120000));
    const Got* second_start = nullptr;
    for (const auto& g : sink.got)
        if (g.id == 2 && g.ev == eb::TtsEvent::Started)
            second_start = &g;
    REQUIRE(second_start != nullptr);
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(second_start->at - finished_first).count() < ebtest::perf_budget(500));
}

TEST_CASE("vieneu real engine reads a heading and the sentence after it as one utterance")
{
    REQUIRE_VIENEU(s);
    Sink sink;
    eb::VieNeuTts tts(s.libs, s.data, s.temp);
    tts.set_sink(sink.hwnd, kMsg);
    REQUIRE(wait_ready(tts));
    REQUIRE(tts.speak(1, "Ch\xC6\xB0\xC6\xA1ng m\xE1\xBB\x99t: L\xC3\xA2m Ph\xC3\xA0m b\xC6\xB0\xE1\xBB\x9B" "c v\xC3\xA0o th\xC3\xA0nh.", kVoice, 1.0, 0.0));
    REQUIRE(sink.wait_for(1, eb::TtsEvent::Finished, 90000));
    size_t started = 0;
    for (const auto& g : sink.got)
        started += g.ev == eb::TtsEvent::Started ? 1 : 0;
    CHECK(started == 1);
}

TEST_CASE("vieneu real engine stop silences events and kills the child; destruction leaves none")
{
    REQUIRE_VIENEU(s);
    Sink sink;
    {
        eb::VieNeuTts tts(s.libs, s.data, s.temp);
        tts.set_sink(sink.hwnd, kMsg);
        REQUIRE(wait_ready(tts));
        REQUIRE(tts.speak(1, kViText, kVoice, 1.0, 0.0));
        tts.prepare(2, kViText, kVoice, 1.0);
        REQUIRE(wait_children(true, 20000));
        tts.stop();
        CHECK(wait_children(false, 10000));
        sink.pump_ms(300);
        CHECK(sink.got.empty());
        REQUIRE(tts.speak(3, kViText, kVoice, 1.0, 0.0));
        REQUIRE(sink.wait_for(3, eb::TtsEvent::Started, 90000));
        CHECK(wait_children(false, 10000));
    }
    const auto t0 = Clock::now();
    {
        eb::VieNeuTts tts(s.libs, s.data, s.temp);
        tts.set_sink(sink.hwnd, kMsg);
        REQUIRE(wait_ready(tts));
        REQUIRE(tts.speak(4, kViText, kVoice, 1.0, 0.0));
        REQUIRE(wait_children(true, 20000));
    }
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count() < ebtest::perf_budget(25000));
    CHECK(wait_children(false, 5000));
}

namespace {

using namespace eb::vieneu::engine;

struct EngineRig {
    Engine e;
    EngineRig()
    {
        e.sh = std::make_shared<Shared>();
        e.queue = std::make_shared<Queue>();
        e.verdict = std::make_shared<Verdict>();
        e.cfg = eb::vieneu::make_cfg(fs::temp_directory_path() / L"eb-vieneu-rig-libs", fs::temp_directory_path() / L"eb-vieneu-rig-data",
            fs::temp_directory_path() / L"eb-vieneu-rig");
    }
    ReqPtr add(uint32_t id, const std::string& text, bool queued)
    {
        auto req = std::make_shared<const Req>(Req{1, 0, id, text, L"v", 1.0, 0.0});
        REQUIRE(add_item(e, req, false));
        e.items.at(id).queued = queued;
        return req;
    }
    void start_batch_of(std::initializer_list<uint32_t> ids)
    {
        Batch b;
        b.dir = fs::temp_directory_path() / L"eb-vieneu-rig-batch";
        for (const uint32_t id : ids)
            b.pieces.push_back({id, 0, false});
        e.batch = std::move(b);
    }
};

bool make_junction(const fs::path& link, const fs::path& target)
{
    const std::wstring cmd = L"cmd.exe /c mklink /J \"" + link.wstring() + L"\" \"" + target.wstring() + L"\" >nul 2>&1";
    return _wsystem(cmd.c_str()) == 0;
}

}

TEST_CASE("vieneu engine: aborting a batch re-queues its items and schedules the next batch")
{
    EngineRig r;
    r.add(1, "One.", true);
    r.e.want = r.add(2, "Two.", true);
    r.e.items.at(1).got = 1;
    r.start_batch_of({1, 2});
    abort_batch(r.e);
    CHECK_FALSE(r.e.batch.has_value());
    REQUIRE(r.e.items.size() == 2);
    CHECK_FALSE(r.e.items.at(1).queued);
    CHECK_FALSE(r.e.items.at(2).queued);
    CHECK(r.e.items.at(1).got == 0);
    CHECK(r.e.start_at.has_value());
    CHECK(r.e.want != nullptr);
}

TEST_CASE("vieneu engine: a broken item fails alone and the running batch survives")
{
    EngineRig r;
    r.add(1, "One.", true);
    r.e.want = r.add(2, "Two.", false);
    r.e.items.at(1).raw[0] = fs::temp_directory_path() / L"eb-vieneu-rig-missing.wav";
    r.e.items.at(1).got = 1;
    r.start_batch_of({1});
    finalize_item(r.e, 1);
    CHECK(r.e.batch.has_value());
    CHECK(r.e.items.count(1) == 0);
    REQUIRE(r.e.items.count(2) == 1);
    CHECK_FALSE(r.e.items.at(2).queued);
    CHECK(r.e.want != nullptr);
}

TEST_CASE("vieneu engine: changing the text of a queued utterance restarts synthesis without losing the waiting one")
{
    EngineRig r;
    r.add(1, "One.", true);
    r.e.want = r.add(2, "Two.", true);
    r.start_batch_of({1, 2});
    const auto changed = std::make_shared<const Req>(Req{1, 0, 2, "Two, but different.", L"v", 1.0, 0.0});
    REQUIRE(add_item(r.e, changed, false));
    CHECK_FALSE(r.e.batch.has_value());
    REQUIRE(r.e.items.size() == 2);
    CHECK(r.e.items.at(2).lines[0] == "Two, but different.");
    CHECK_FALSE(r.e.items.at(2).queued);
    CHECK(r.e.start_at.has_value());
    CHECK(r.e.want != nullptr);
}

TEST_CASE("vieneu engine: stopping clears everything and the next speak starts fresh")
{
    EngineRig r;
    r.add(1, "One.", true);
    r.e.want = r.add(2, "Two.", false);
    r.start_batch_of({1});
    r.e.fresh = false;
    do_stop(r.e);
    CHECK(r.e.items.empty());
    CHECK_FALSE(r.e.batch.has_value());
    CHECK_FALSE(r.e.start_at.has_value());
    CHECK(r.e.want == nullptr);
    CHECK(r.e.fresh);
}

TEST_CASE("remove_tree deletes real folders but only unlinks junctions")
{
    const TmpDir t(L"eb-vieneu-removetree");
    const fs::path sentinel = t.dir / L"sentinel";
    write_file(sentinel / L"keep.txt", "keep");
    write_file(t.dir / L"victim" / L"a" / L"b.txt", "x");
    if (!make_junction(t.dir / L"victim" / L"link", sentinel)) {
        MESSAGE("junction creation not permitted, test skipped");
        return;
    }
    eb::proc::remove_tree(t.dir / L"victim");
    CHECK_FALSE(fs::exists(t.dir / L"victim"));
    CHECK(fs::exists(sentinel / L"keep.txt"));
    eb::proc::remove_tree(t.dir / L"never-existed");
}
