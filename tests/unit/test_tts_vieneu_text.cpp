#include "tts/vieneu/VieNeuText.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <fstream>

namespace {

namespace fs = std::filesystem;
using namespace eb::vieneu;

void put32(std::string& s, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

void put16(std::string& s, uint16_t v)
{
    s.push_back(static_cast<char>(v & 0xFF));
    s.push_back(static_cast<char>(v >> 8));
}

std::string make_wav(uint32_t rate, uint16_t channels, const std::string& pcm, uint16_t bits = 16, uint16_t fmt = 1)
{
    std::string w = "RIFF";
    put32(w, static_cast<uint32_t>(36 + pcm.size()));
    w += "WAVEfmt ";
    put32(w, 16);
    put16(w, fmt);
    put16(w, channels);
    put32(w, rate);
    put32(w, rate * channels * bits / 8);
    put16(w, static_cast<uint16_t>(channels * bits / 8));
    put16(w, bits);
    w += "data";
    put32(w, static_cast<uint32_t>(pcm.size()));
    return w + pcm;
}

struct TmpDir {
    fs::path dir = fs::temp_directory_path() / L"eb-vieneu-text-test";
    TmpDir()
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

void write_file(const fs::path& p, const std::string& content)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << content;
}

void add_preset(const fs::path& voices, const std::string& id, size_t emb = 2200, size_t codes = 3000)
{
    write_file(voices / id / "speaker.emb.txt", std::string(emb, '1'));
    write_file(voices / id / "ref_codes.txt", std::string(codes, '2'));
}

}

TEST_CASE("vieneu heading: a short heading ending in a colon is split off")
{
    const auto h = split_heading("Ch\xC6\xB0\xC6\xA1ng m\xE1\xBB\x99t: L\xC3\xA2m Ph\xC3\xA0m b\xC6\xB0\xE1\xBB\x9B" "c v\xC3\xA0o th\xC3\xA0nh.");
    REQUIRE(h.has_value());
    CHECK(h->head == "Ch\xC6\xB0\xC6\xA1ng m\xE1\xBB\x99t.");
    CHECK(h->rest == "L\xC3\xA2m Ph\xC3\xA0m b\xC6\xB0\xE1\xBB\x9B" "c v\xC3\xA0o th\xC3\xA0nh.");
    const auto two = split_heading("  Chapter 1:   It begins.");
    REQUIRE(two.has_value());
    CHECK(two->head == "Chapter 1.");
    CHECK(two->rest == "It begins.");
}

TEST_CASE("vieneu heading: anything that is not a short heading is left alone")
{
    CHECK_FALSE(split_heading("No colon in this sentence."));
    CHECK_FALSE(split_heading("It is 10:30 now and we are late."));
    CHECK_FALSE(split_heading("One two three four five six: too long a heading."));
    CHECK_FALSE(split_heading("Chapter one:"));
    CHECK_FALSE(split_heading("Chapter one:   "));
    CHECK_FALSE(split_heading(": starts with a colon"));
    CHECK_FALSE(split_heading("He said. Then: nothing."));
    CHECK_FALSE(split_heading("Really? Yes: maybe."));
    CHECK_FALSE(split_heading(std::string(300, 'a') + ": long word before colon"));
    CHECK_FALSE(split_heading(""));
}

TEST_CASE("vieneu batch line: controls become spaces, spaces collapse, invalid UTF-8 is not passed on")
{
    CHECK(batch_line("  hello\r\nworld\t!  ") == "hello world !");
    CHECK(batch_line(std::string("a\x01" "b\x7F" "c")) == "a b c");
    CHECK(batch_line("x\xC2\x85y\xE2\x80\xA8z") == "x y z");
    CHECK(batch_line("bad\xFF" "byte \xC3") == "bad byte");
    CHECK(batch_line("zero\xE2\x80\x8Bwidth\xEF\xBB\xBF") == "zerowidth");
    CHECK(batch_line("Tr\xC4\x83m n\xC4\x83m") == "Tr\xC4\x83m n\xC4\x83m");
    CHECK(batch_line("").empty());
    CHECK(batch_line(std::string("\x01\x02 \r\n\xFF")).empty());
}

TEST_CASE("vieneu batch file: LF separated, no byte order mark, no trailing blank line")
{
    CHECK(batch_file({"one", "two", "three"}) == "one\ntwo\nthree\n");
    CHECK(batch_file({"only"}) == "only\n");
    CHECK(batch_file({}).empty());
}

TEST_CASE("vieneu wav: header parsing is bounded")
{
    const std::string pcm(400, '\x05');
    const auto ok = parse_wav(make_wav(48000, 2, pcm));
    REQUIRE(ok.has_value());
    CHECK(ok->rate == 48000);
    CHECK(ok->channels == 2);
    CHECK(ok->data_off == 44);
    CHECK(ok->data_len == 400);

    const std::string full = make_wav(48000, 2, pcm);
    const auto cut = parse_wav(std::string_view(full).substr(0, 44 + 203));
    REQUIRE(cut.has_value());
    CHECK(cut->data_len == 200);

    CHECK_FALSE(parse_wav(""));
    CHECK_FALSE(parse_wav(std::string_view(full).substr(0, 30)));
    CHECK_FALSE(parse_wav(make_wav(48000, 2, pcm, 8)));
    CHECK_FALSE(parse_wav(make_wav(48000, 2, pcm, 16, 3)));
    CHECK_FALSE(parse_wav(make_wav(48000, 3, pcm)));
    CHECK_FALSE(parse_wav(make_wav(100, 1, pcm)));
    CHECK_FALSE(parse_wav(make_wav(400000, 1, pcm)));
    std::string no_riff = full;
    no_riff[0] = 'X';
    CHECK_FALSE(parse_wav(no_riff));
    std::string huge_chunk = full;
    huge_chunk[16] = '\xFF';
    huge_chunk[17] = '\xFF';
    huge_chunk[18] = '\xFF';
    huge_chunk[19] = '\x7F';
    CHECK_FALSE(parse_wav(huge_chunk));
}

TEST_CASE("vieneu wav: data chunk before fmt is rejected, unknown chunks are skipped")
{
    std::string w = "RIFF";
    put32(w, 100);
    w += "WAVEdata";
    put32(w, 4);
    w += "abcd";
    CHECK_FALSE(parse_wav(w));

    std::string v = "RIFF";
    put32(v, 100);
    v += "WAVELIST";
    put32(v, 3);
    v += "xyz";
    v.push_back('\0');
    v += "fmt ";
    put32(v, 16);
    put16(v, 1);
    put16(v, 1);
    put32(v, 24000);
    put32(v, 48000);
    put16(v, 2);
    put16(v, 16);
    v += "data";
    put32(v, 6);
    v += "123456";
    const auto info = parse_wav(v);
    REQUIRE(info.has_value());
    CHECK(info->rate == 24000);
    CHECK(info->data_len == 6);
}

TEST_CASE("vieneu wav: join inserts silence of the requested length before each part")
{
    const std::string a = make_wav(48000, 2, std::string(800, '\x01'));
    const std::string b = make_wav(48000, 2, std::string(400, '\x02'));
    const std::vector<WavPart> parts = {{a, 300}, {b, 300}};
    const auto joined = join_wavs(parts);
    REQUIRE(joined.has_value());
    const auto info = parse_wav(*joined);
    REQUIRE(info.has_value());
    const size_t silence = size_t{48000} * 300 / 1000 * 4;
    CHECK(info->data_len == silence + 800 + silence + 400);
    CHECK(joined->size() == 44 + info->data_len);
    CHECK((*joined)[info->data_off] == '\0');
    CHECK((*joined)[info->data_off + silence] == '\x01');
    CHECK((*joined)[info->data_off + silence + 800 + silence] == '\x02');

    const auto plain = join_wavs({{a, 0}});
    REQUIRE(plain.has_value());
    CHECK(plain->size() == a.size());
}

TEST_CASE("vieneu wav: join refuses mismatched or broken parts and oversize output")
{
    const std::string a = make_wav(48000, 2, std::string(8, 'a'));
    CHECK_FALSE(join_wavs({}));
    CHECK_FALSE(join_wavs({{a, 0}, {make_wav(24000, 2, "abcd"), 0}}));
    CHECK_FALSE(join_wavs({{a, 0}, {make_wav(48000, 1, "abcd"), 0}}));
    CHECK_FALSE(join_wavs({{a, 0}, {"junk", 0}}));
    CHECK_FALSE(join_wavs({{a, 4000000}}));
}

TEST_CASE("vieneu presets: found by their two files, named from the manifest")
{
    const TmpDir t;
    const fs::path v = t.dir / L"voices";
    add_preset(v, "minh_quan_pro");
    add_preset(v, "ngoc_huyen");
    add_preset(v, "adam");
    write_file(v / "manifest.json",
        "{\"default\":\"minh_quan_pro\",\"voices\":[{\"id\":\"ngoc_huyen\",\"name\":\"Ng\xE1\xBB\x8D" "c Huy\xE1\xBB\x81n\","
        "\"label\":\"x\"},{\"id\":\"minh_quan_pro\",\"name\":\"Minh Qu\xC3\xA2n Pro\"}]}");
    const auto list = discover_presets(v);
    REQUIRE(list.size() == 3);
    bool nh = false;
    bool mq = false;
    bool adam = false;
    for (const auto& p : list) {
        nh = nh || (p.id == L"ngoc_huyen" && p.name == L"Ngọc Huyền");
        mq = mq || (p.id == L"minh_quan_pro" && p.name == L"Minh Quân Pro");
        adam = adam || (p.id == L"adam" && p.name == L"adam");
        CHECK(fs::is_regular_file(p.emb));
        CHECK(fs::is_regular_file(p.codes));
    }
    CHECK(nh);
    CHECK(mq);
    CHECK(adam);
}

TEST_CASE("vieneu presets: broken, oversize, oddly named and missing entries are skipped")
{
    const TmpDir t;
    const fs::path v = t.dir / L"voices";
    add_preset(v, "good");
    add_preset(v, "empty_emb", 0, 100);
    add_preset(v, "huge_codes", 2200, size_t{5} * 1024 * 1024);
    add_preset(v, "huge_emb", size_t{1} << 20, 100);
    add_preset(v, "bad name!");
    add_preset(v, "has.dot");
    write_file(v / "only_emb" / "speaker.emb.txt", std::string(2200, '1'));
    write_file(v / "stray.txt", "x");
    const auto list = discover_presets(v);
    REQUIRE(list.size() == 1);
    CHECK(list[0].id == L"good");
    CHECK(discover_presets(t.dir / L"nowhere").empty());
}

TEST_CASE("vieneu presets: an unreadable or hostile manifest only costs the display names")
{
    const TmpDir t;
    const fs::path v = t.dir / L"voices";
    add_preset(v, "a");
    write_file(v / "manifest.json", std::string(200000, '{'));
    auto list = discover_presets(v);
    REQUIRE(list.size() == 1);
    CHECK(list[0].name == L"a");
    write_file(v / "manifest.json", "{\"voices\":[{\"id\":\"a\",\"name\":\"bad\\u0041 \\\"name\"}]}");
    list = discover_presets(v);
    REQUIRE(list.size() == 1);
    CHECK(list[0].name == L"a");
}

TEST_CASE("vieneu batch file: hostile text cannot shift the line numbers audiocpp_cli uses as request ids")
{
    // audiocpp_cli v0.9.3 --batch-text-file: one request per non-empty line, id line_<file line number>, only CR trimmed.
    const std::vector<std::string> texts = {"a\r\n\r\nb", "#comment\n// x", "\xE2\x80\xA8\xE2\x80\xA9" "tail", "line\x0B" "feed\x0C" "x"};
    std::vector<std::string> lines;
    for (const std::string& t : texts) {
        const std::string l = batch_line(t);
        REQUIRE_FALSE(l.empty());
        CHECK(l.find_first_of("\r\n\x0B\x0C") == std::string::npos);
        lines.push_back(l);
    }
    const std::string file = batch_file(lines);
    CHECK(static_cast<size_t>(std::count(file.begin(), file.end(), '\n')) == texts.size());
}
