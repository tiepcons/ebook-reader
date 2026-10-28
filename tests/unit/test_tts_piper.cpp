#include "tts/piper/PiperTts.h"

#include "base/PathSafety.h"
#include "tts/Utf8.h"

#include <doctest/doctest.h>

#include <tlhelp32.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
constexpr UINT kMsg = WM_APP + 9;

struct Got {
    uint32_t id;
    eb::TtsEvent ev;
    uint32_t value;
    Clock::time_point at;
};

std::vector<Got>* g_got = nullptr;
std::function<void(const Got&)> g_on_event;

LRESULT CALLBACK sink_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == kMsg) {
        if (g_got != nullptr) {
            g_got->push_back({static_cast<uint32_t>(w), eb::tts_event(l), eb::tts_value(l), Clock::now()});
            if (g_on_event)
                g_on_event(g_got->back());
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

struct Sink {
    HWND hwnd = nullptr;
    std::vector<Got> got;

    Sink()
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = sink_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"EbPiperTestSink";
        RegisterClassW(&wc);
        hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        g_got = &got;
    }
    ~Sink()
    {
        g_got = nullptr;
        g_on_event = nullptr;
        DestroyWindow(hwnd);
    }
    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;

    static void pump_pending()
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            DispatchMessageW(&msg);
    }
    void pump_ms(int ms)
    {
        const auto end = Clock::now() + std::chrono::milliseconds(ms);
        while (Clock::now() < end) {
            pump_pending();
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        }
    }
    bool wait_for(uint32_t id, eb::TtsEvent ev, int timeout_ms)
    {
        const auto end = Clock::now() + std::chrono::milliseconds(timeout_ms);
        while (Clock::now() < end) {
            pump_pending();
            for (const auto& g : got)
                if (g.id == id && (g.ev == ev || g.ev == eb::TtsEvent::Error))
                    return g.ev == ev;
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        }
        return false;
    }
};

struct Paths {
    fs::path piper = EB_PIPER_DIR;
    fs::path voices = EB_PIPER_VOICES;
    fs::path temp = fs::temp_directory_path() / L"eb-piper-test";
    bool present() const
    {
        std::error_code ec;
        return fs::exists(piper / L"piper.exe", ec) && fs::exists(voices / L"vi_VN-vais1000-medium.onnx", ec)
            && fs::exists(voices / L"en_US-lessac-medium.onnx", ec);
    }
};

const std::wstring kVi = L"vi_VN-vais1000-medium";
const std::wstring kEn = L"en_US-lessac-medium";

std::vector<DWORD> piper_children()
{
    std::vector<DWORD> out;
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return out;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (pe.th32ParentProcessID == GetCurrentProcessId() && std::wstring_view(pe.szExeFile) == L"piper.exe")
            out.push_back(pe.th32ProcessID);
    CloseHandle(snap);
    return out;
}

bool wait_children(bool present, int timeout_ms)
{
    const auto end = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < end) {
        if (piper_children().empty() != present)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

#define REQUIRE_PIPER(p)                                                                                              \
    const Paths p;                                                                                                    \
    if (!p.present()) {                                                                                               \
        MESSAGE("piper spike binaries or voices missing, skipped");                                                   \
        return;                                                                                                       \
    }

std::string long_text()
{
    std::string text;
    while (text.size() < 1500)
        text += "The quick brown fox jumps over the lazy dog while the band plays on. ";
    return text;
}

}

TEST_CASE("tts piper json escaping")
{
    CHECK(eb::piper::json_escape("a\"b\\c\nd\te\r") == "a\\\"b\\\\c\\nd\\te\\r");
    CHECK(eb::piper::json_escape(std::string("x\x01y")) == "x\\u0001y");
    CHECK(eb::piper::json_escape("Tr\xC4\x83m n\xC4\x83m") == "Tr\xC4\x83m n\xC4\x83m");
    CHECK(eb::piper::json_escape("bad\xFF" "byte") == "bad\\ufffdbyte");
    CHECK(eb::piper::request_line("say \"hi\"\n", "C:\\t\\a.wav")
        == "{\"text\":\"say \\\"hi\\\"\\n\",\"output_file\":\"C:\\\\t\\\\a.wav\"}");
}

TEST_CASE("tts piper command line quoting and language code")
{
    CHECK(eb::piper::quote_arg(L"plain") == L"plain");
    CHECK(eb::piper::quote_arg(L"C:\\Program Files\\x") == L"\"C:\\Program Files\\x\"");
    CHECK(eb::piper::quote_arg(L"a\"b") == L"\"a\\\"b\"");
    CHECK(eb::piper::quote_arg(L"dir \\") == L"\"dir \\\\\"");
    CHECK(eb::piper::lang_from_model_json("{\"audio\":{},\"language\":{\"code\":\"vi_VN\",\"family\":\"vi\"}}") == L"vi-VN");
    CHECK(eb::piper::lang_from_model_json("{\"language\":{\"name\":\"x\"},\"code\":\"zz\"}").empty());
    CHECK(eb::piper::lang_from_model_json("{}").empty());
}

TEST_CASE("tts piper voices lists vi-VN and en-US")
{
    REQUIRE_PIPER(p);
    eb::PiperTts tts(p.piper, p.voices, p.temp);
    bool vi = false;
    bool en = false;
    for (const auto& v : tts.voices()) {
        vi = vi || (v.id == kVi && v.lang == L"vi-VN");
        en = en || (v.id == kEn && v.lang == L"en-US");
    }
    CHECK(vi);
    CHECK(en);
    eb::PiperTts missing(p.temp / L"none", p.voices, p.temp);
    CHECK(missing.voices().empty());
}

TEST_CASE("tts piper speaks Vietnamese and English")
{
    REQUIRE_PIPER(p);
    Sink sink;
    eb::PiperTts tts(p.piper, p.voices, p.temp);
    tts.set_sink(sink.hwnd, kMsg);
    REQUIRE(tts.speak(1, "Tr\xC4\x83m n\xC4\x83m trong c\xC3\xB5i ng\xC6\xB0\xE1\xBB\x9Di ta.", kVi, 1.0, 0.0));
    REQUIRE(sink.wait_for(1, eb::TtsEvent::Finished, 60000));
    REQUIRE(tts.speak(2, "The quick brown fox jumps over the lazy dog.", kEn, 1.0, 0.0));
    REQUIRE(sink.wait_for(2, eb::TtsEvent::Finished, 60000));
    std::vector<eb::TtsEvent> seq;
    for (const auto& g : sink.got)
        seq.push_back(g.ev);
    const std::vector<eb::TtsEvent> want = {eb::TtsEvent::Started, eb::TtsEvent::Finished, eb::TtsEvent::Started,
        eb::TtsEvent::Finished};
    CHECK(seq == want);
}

TEST_CASE("tts piper unknown voice posts Error")
{
    REQUIRE_PIPER(p);
    Sink sink;
    eb::PiperTts tts(p.piper, p.voices, p.temp);
    tts.set_sink(sink.hwnd, kMsg);
    REQUIRE(tts.speak(5, "hello", L"..\\evil", 1.0, 0.0));
    CHECK(!sink.wait_for(5, eb::TtsEvent::Finished, 10000));
    REQUIRE(!sink.got.empty());
    CHECK(sink.got.back().ev == eb::TtsEvent::Error);
    CHECK(sink.got.back().value == eb::kTtsBadVoice);
}

TEST_CASE("tts piper prepared utterance starts within 300 ms")
{
    REQUIRE_PIPER(p);
    Sink sink;
    eb::PiperTts tts(p.piper, p.voices, p.temp);
    tts.set_sink(sink.hwnd, kMsg);
    const std::string next = "General Kenobi.";
    Clock::time_point t0{};
    g_on_event = [&](const Got& g) {
        if (g.id == 1 && g.ev == eb::TtsEvent::Finished) {
            t0 = Clock::now();
            tts.speak(2, next, kEn, 1.0, 0.0);
        }
    };
    REQUIRE(tts.speak(1, "One two three four five six.", kEn, 1.0, 0.0));
    tts.prepare(2, next, kEn, 1.0);
    const bool st = sink.wait_for(2, eb::TtsEvent::Started, 60000);
    for (const auto& g : sink.got) MESSAGE("ev id=", g.id, " ev=", int(g.ev), " v=", g.value);
    REQUIRE(st);
    const Got* started = nullptr;
    for (const auto& g : sink.got)
        if (g.id == 2 && g.ev == eb::TtsEvent::Started)
            started = &g;
    REQUIRE(started != nullptr);
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(started->at - t0).count() < 300);
    CHECK(sink.wait_for(2, eb::TtsEvent::Finished, 30000));
}

TEST_CASE("tts piper stop silences further events")
{
    REQUIRE_PIPER(p);
    Sink sink;
    eb::PiperTts tts(p.piper, p.voices, p.temp);
    tts.set_sink(sink.hwnd, kMsg);
    REQUIRE(tts.speak(1, "One two three four five six seven eight nine ten eleven twelve.", kEn, 1.0, 0.0));
    REQUIRE(sink.wait_for(1, eb::TtsEvent::Started, 60000));
    sink.pump_ms(300);
    tts.stop();
    Sink::pump_pending();
    sink.got.clear();
    sink.pump_ms(1500);
    CHECK(sink.got.empty());
}

TEST_CASE("tts piper stop during synthesis then speak again")
{
    REQUIRE_PIPER(p);
    Sink sink;
    eb::PiperTts tts(p.piper, p.voices, p.temp);
    tts.set_sink(sink.hwnd, kMsg);
    REQUIRE(tts.speak(1, long_text(), kEn, 1.0, 0.0));
    sink.pump_ms(900);
    tts.stop();
    Sink::pump_pending();
    sink.got.clear();
    REQUIRE(tts.speak(2, "Short one.", kEn, 1.0, 0.0));
    REQUIRE(sink.wait_for(2, eb::TtsEvent::Finished, 60000));
    for (const auto& g : sink.got)
        CHECK(g.id == 2);
}

TEST_CASE("tts piper recovers after the child process is killed")
{
    REQUIRE_PIPER(p);
    Sink sink;
    eb::PiperTts tts(p.piper, p.voices, p.temp);
    tts.set_sink(sink.hwnd, kMsg);
    REQUIRE(tts.speak(1, long_text(), kEn, 1.0, 0.0));
    REQUIRE(wait_children(true, 10000));
    sink.pump_ms(800);
    for (const DWORD pid : piper_children()) {
        const HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        REQUIRE(h != nullptr);
        TerminateProcess(h, 1);
        CloseHandle(h);
    }
    CHECK(!sink.wait_for(1, eb::TtsEvent::Finished, 20000));
    REQUIRE(!sink.got.empty());
    CHECK(sink.got.back().ev == eb::TtsEvent::Error);
    CHECK(sink.got.back().value == eb::kTtsSynthFailed);
    sink.got.clear();
    REQUIRE(tts.speak(2, "Back again.", kEn, 1.0, 0.0));
    CHECK(sink.wait_for(2, eb::TtsEvent::Finished, 60000));
}

TEST_CASE("tts piper destruction while speaking is quick and leaves no child")
{
    REQUIRE_PIPER(p);
    Sink sink;
    Clock::time_point t0{};
    {
        eb::PiperTts tts(p.piper, p.voices, p.temp);
        tts.set_sink(sink.hwnd, kMsg);
        REQUIRE(tts.speak(1, long_text(), kEn, 1.0, 0.0));
        tts.prepare(2, "Next.", kEn, 1.0);
        REQUIRE(wait_children(true, 10000));
        sink.pump_ms(300);
        t0 = Clock::now();
    }
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count() < 1000);
    CHECK(wait_children(false, 2000));
}

namespace {

bool decodes_as_invalid(const std::string& s)
{
    const auto c = eb::u8::decode(s, 0);
    return c.cp == 0xFFFD && c.len == 1;
}

char32_t decoded(const std::string& s, uint32_t len)
{
    const auto c = eb::u8::decode(s, 0);
    return c.len == len ? c.cp : U'?';
}

}

TEST_CASE("tts utf8 decoder is strict")
{
    CHECK(decodes_as_invalid("\xC0\x80"));
    CHECK(decodes_as_invalid("\xC1\xBF"));
    CHECK(decodes_as_invalid("\xE0\x9F\xBF"));
    CHECK(decodes_as_invalid("\xE0\x80\x80"));
    CHECK(decodes_as_invalid("\xED\xA0\x80"));
    CHECK(decodes_as_invalid("\xED\xBF\xBF"));
    CHECK(decodes_as_invalid("\xF0\x8F\xBF\xBF"));
    CHECK(decodes_as_invalid("\xF4\x90\x80\x80"));
    CHECK(decodes_as_invalid("\xF5\x80\x80\x80"));
    CHECK(decodes_as_invalid("\xF8\x88\x80\x80\x80"));
    CHECK(decodes_as_invalid("\x80"));
    CHECK(decodes_as_invalid("\xC2"));
    CHECK(decodes_as_invalid("\xE2\x82"));
    CHECK(decodes_as_invalid("\xF0\x9F\x98"));
    CHECK(decoded("\xE0\xA0\x80", 3) == 0x800);
    CHECK(decoded("\xED\x9F\xBF", 3) == 0xD7FF);
    CHECK(decoded("\xEE\x80\x80", 3) == 0xE000);
    CHECK(decoded("\xF0\x90\x80\x80", 4) == 0x10000);
    CHECK(decoded("\xF4\x8F\xBF\xBF", 4) == 0x10FFFF);
}

TEST_CASE("tts piper json escapes invalid UTF-8 and line separators")
{
    CHECK(eb::piper::json_escape("a\xED\xA0\x80z") == "a\\ufffd\\ufffd\\ufffdz");
    CHECK(eb::piper::json_escape("\xF4\x90\x80\x80") == "\\ufffd\\ufffd\\ufffd\\ufffd");
    CHECK(eb::piper::json_escape("\xC0\xAF") == "\\ufffd\\ufffd");
    CHECK(eb::piper::json_escape("x\xE2\x80\xA8y\xE2\x80\xA9") == "x\\u2028y\\u2029");
}

TEST_CASE("tts piper temp dir must not sit behind a reparse point")
{
    const fs::path base = fs::temp_directory_path() / L"eb-piper-junction";
    std::error_code ec;
    fs::create_directories(base / L"real", ec);
    const fs::path link = base / L"link";
    fs::remove(link, ec);
    CHECK(eb::is_plain_dir_chain(base / L"real"));
    CHECK(!eb::is_plain_dir_chain(base / L"missing"));
    const std::wstring cmd = L"cmd /c mklink /J \"" + link.wstring() + L"\" \"" + (base / L"real").wstring() + L"\" >nul";
    if (_wsystem(cmd.c_str()) != 0) {
        MESSAGE("mklink /J unavailable, junction check skipped");
        return;
    }
    CHECK(!eb::is_plain_dir_chain(link));
    CHECK(!eb::is_plain_dir_chain(link / L"sub"));
    fs::remove(link, ec);
}

TEST_CASE("tts piper destruction with a wedged child is quick")
{
    const fs::path fake = EB_FAKE_PIPER_DIR;
    std::error_code ec;
    if (!fs::exists(fake / L"piper.exe", ec)) {
        MESSAGE("fake piper missing, skipped");
        return;
    }
    const fs::path voices = fs::temp_directory_path() / L"eb-fake-voices";
    fs::create_directories(voices, ec);
    for (const wchar_t* name : {L"x.onnx", L"x.onnx.json"})
        std::ofstream(voices / name, std::ios::binary) << "{}";
    Sink sink;
    Clock::time_point t0{};
    {
        eb::PiperTts tts(fake, voices, fs::temp_directory_path() / L"eb-fake-temp");
        tts.set_sink(sink.hwnd, kMsg);
        REQUIRE(tts.speak(1, std::string(60000, 'a') + ".", L"x", 1.0, 0.0));
        tts.prepare(2, "Next.", L"x", 1.0);
        REQUIRE(wait_children(true, 10000));
        sink.pump_ms(300);
        t0 = Clock::now();
    }
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count() < 1000);
    CHECK(wait_children(false, 2000));
}
