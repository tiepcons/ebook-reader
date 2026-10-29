#include "tts/vieneu/VieNeuProc.h"
#include "tts/vieneu/VieNeuTts.h"

#include "tts_sink.h"

#include <doctest/doctest.h>

#include <tlhelp32.h>

#include <chrono>
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

    Fixture()
    {
        write_file(libs / L"audiocpp_cli.exe", "MZ", kMiB);
        write_file(libs / L"sea_g2p_rs.dll", "MZ", kMiB);
        write_file(data / L"vieneu-v3-turbo-q8_0.gguf", "GGUF", 40 * kMiB);
        write_file(data / L"sea_g2p.bin", "x", 8 * kMiB);
        add_preset(data / L"voices", "ngoc_huyen");
        add_preset(data / L"voices", "minh_quan_pro");
    }
};

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
    CHECK(cmd.find(L"g2p_dict_file=") != std::wstring::npos);
    CHECK(cmd.find(L"g2p_library=") != std::wstring::npos);
    CHECK(cmd.find(L"speaker_embedding_file=") != std::wstring::npos);
    CHECK(cmd.find(L"reference_codes_file=") != std::wstring::npos);
    CHECK(cmd.find(L"--server") == std::wstring::npos);
    CHECK(cmd.find(L"--port") == std::wstring::npos);
    const unsigned t = eb::vieneu::child_threads();
    CHECK(t >= 1);
    CHECK(t <= 4);
}

TEST_CASE("vieneu voices: every preset is offered for Vietnamese and English; nothing without the model")
{
    const Fixture f;
    eb::VieNeuTts tts(f.libs, f.data, f.t.dir / L"tmp");
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
    std::error_code ec;
    fs::remove(f.data / L"sea_g2p.bin", ec);
    CHECK(tts.voices().empty());
}

TEST_CASE("vieneu engine without a runtime fails the utterance instead of hanging")
{
    const Fixture f;
    Sink sink;
    eb::VieNeuTts tts(f.libs, f.data, f.t.dir / L"tmp");
    tts.set_sink(sink.hwnd, kMsg);
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
    eb::VieNeuTts tts(f.libs, f.data, f.t.dir / L"tmp");
    tts.set_sink(sink.hwnd, kMsg);
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
    size_t wav_bytes = 0;
    g_on_event = [&](const Got& g) {
        if (g.ev == eb::TtsEvent::Started)
            wav_bytes = largest_wav_bytes(s.temp);
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
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(second_start->at - finished_first).count() < 500);
}

TEST_CASE("vieneu real engine reads a heading and the sentence after it as one utterance")
{
    REQUIRE_VIENEU(s);
    Sink sink;
    eb::VieNeuTts tts(s.libs, s.data, s.temp);
    tts.set_sink(sink.hwnd, kMsg);
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
        REQUIRE(tts.speak(4, kViText, kVoice, 1.0, 0.0));
        REQUIRE(wait_children(true, 20000));
    }
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count() < 25000);
    CHECK(wait_children(false, 5000));
}
