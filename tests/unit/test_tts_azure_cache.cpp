#include "tts/azure/AudioCache.h"

#include "test_base_tmp.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <fstream>

namespace {

using std::chrono::hours;
namespace fs = std::filesystem;

std::vector<std::byte> blob(size_t n, unsigned char fill)
{
    return std::vector<std::byte>(n, static_cast<std::byte>(fill));
}

std::string key(int i)
{
    return eb::AudioCache::key_for("vi-VN-HoaiMyNeural", 0, "sentence " + std::to_string(i));
}

fs::path file_of(const ebtest::TempDir& t, const std::string& k)
{
    return t.path() / (std::wstring(k.begin(), k.end()) + L".mp3");
}

void age(const ebtest::TempDir& t, const std::string& k, int hours_ago)
{
    std::error_code ec;
    fs::last_write_time(file_of(t, k), fs::file_time_type::clock::now() - hours(hours_ago), ec);
    REQUIRE_FALSE(ec);
}

bool make_junction(const fs::path& link, const fs::path& target)
{
    const std::string cmd = "cmd /c mklink /J \"" + link.string() + "\" \"" + target.string() + "\" >nul 2>&1";
    return std::system(cmd.c_str()) == 0;
}

size_t count_files(const fs::path& dir)
{
    size_t n = 0;
    for (const auto& e : fs::directory_iterator(dir))
        n += e.is_regular_file() ? 1 : 0;
    return n;
}

}

TEST_CASE("tts azure cache key hashes voice, rate and text")
{
    const auto k = eb::AudioCache::key_for("vi-VN-HoaiMyNeural", 0, "xin chao");
    CHECK(k.size() == 64);
    CHECK(k == eb::AudioCache::key_for("vi-VN-HoaiMyNeural", 0, "xin chao"));
    CHECK(k != eb::AudioCache::key_for("vi-VN-NamMinhNeural", 0, "xin chao"));
    CHECK(k != eb::AudioCache::key_for("vi-VN-HoaiMyNeural", 10, "xin chao"));
    CHECK(k != eb::AudioCache::key_for("vi-VN-HoaiMyNeural", 0, "xin chao."));
    for (const char c : k)
        CHECK(((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')));
}

TEST_CASE("tts azure cache stores atomically and rejects bad keys")
{
    ebtest::TempDir t;
    eb::AudioCache cache(t.path(), 1000);
    const auto k = key(1);
    CHECK_FALSE(cache.get(k).has_value());
    CHECK(cache.put(k, blob(100, 7)));
    const auto hit = cache.get(k);
    REQUIRE(hit.has_value());
    CHECK(*hit == blob(100, 7));
    CHECK(cache.put(k, blob(40, 9)));
    CHECK(cache.total_bytes() == 40);
    CHECK(*cache.get(k) == blob(40, 9));
    CHECK(count_files(t.path()) == 1);

    CHECK_FALSE(cache.put("../evil", blob(10, 1)));
    CHECK_FALSE(cache.put(std::string(64, 'Z'), blob(10, 1)));
    CHECK_FALSE(cache.get("..\\..\\x").has_value());
    CHECK_FALSE(cache.put(key(2), {}));
    CHECK(count_files(t.path()) == 1);
}

TEST_CASE("tts azure cache evicts the least recently used entry after a write")
{
    ebtest::TempDir t;
    eb::AudioCache cache(t.path(), 250);
    const auto k1 = key(1);
    const auto k2 = key(2);
    const auto k3 = key(3);
    REQUIRE(cache.put(k1, blob(100, 1)));
    REQUIRE(cache.put(k2, blob(100, 2)));
    age(t, k1, 10);
    age(t, k2, 5);
    REQUIRE(cache.get(k1).has_value());
    REQUIRE(cache.put(k3, blob(100, 3)));
    CHECK(cache.get(k1).has_value());
    CHECK_FALSE(cache.get(k2).has_value());
    CHECK(cache.get(k3).has_value());
    CHECK(cache.total_bytes() == 200);
}

TEST_CASE("tts azure cache enforces the cap at startup and drops leftover temp files")
{
    ebtest::TempDir t;
    const auto k1 = key(1);
    const auto k2 = key(2);
    const auto k3 = key(3);
    {
        eb::AudioCache cache(t.path(), 10000);
        REQUIRE(cache.put(k1, blob(100, 1)));
        REQUIRE(cache.put(k2, blob(100, 2)));
        REQUIRE(cache.put(k3, blob(100, 3)));
    }
    age(t, k3, 30);
    age(t, k1, 20);
    age(t, k2, 10);
    std::ofstream(t.path() / (std::wstring(k1.begin(), k1.end()) + L".mp3.tmp42"), std::ios::binary) << "partial";
    std::ofstream(t.path() / L"notes.txt") << "keep";
    eb::AudioCache reopened(t.path(), 150);
    CHECK(reopened.total_bytes() == 100);
    CHECK(reopened.get(k2).has_value());
    CHECK_FALSE(reopened.get(k1).has_value());
    CHECK_FALSE(reopened.get(k3).has_value());
    CHECK(count_files(t.path()) == 2);
    CHECK(fs::exists(t.path() / L"notes.txt"));
}

TEST_CASE("tts azure cache is disabled when its directory is a junction")
{
    ebtest::TempDir t;
    const fs::path real = t.path() / L"real";
    const fs::path link = t.path() / L"link";
    fs::create_directories(real);
    REQUIRE(make_junction(link, real));
    const auto k = key(1);
    {
        eb::AudioCache direct(real, 1000);
        REQUIRE(direct.put(k, blob(10, 1)));
    }
    eb::AudioCache cache(link, 1000);
    CHECK_FALSE(cache.put(key(2), blob(10, 2)));
    CHECK_FALSE(cache.get(k).has_value());
    CHECK(cache.total_bytes() == 0);
    CHECK(count_files(real) == 1);
}

TEST_CASE("tts azure cache touches only its own file names")
{
    ebtest::TempDir t;
    const auto k = key(1);
    const std::string foreign = "precious.mp3";
    std::ofstream(t.path() / foreign, std::ios::binary) << std::string(500, 'x');
    std::ofstream(t.path() / (std::wstring(k.begin(), k.end()) + L".mp3.tmp"), std::ios::binary) << "no digits";
    std::ofstream(t.path() / L"other.tmp7", std::ios::binary) << "foreign temp";
    const auto dir_key = key(9);
    fs::create_directories(t.path() / (std::wstring(dir_key.begin(), dir_key.end()) + L".mp3"));
    eb::AudioCache cache(t.path(), 100);
    CHECK(cache.total_bytes() == 0);
    CHECK(fs::exists(t.path() / foreign));
    CHECK(fs::exists(t.path() / L"other.tmp7"));
    CHECK(fs::exists(t.path() / (std::wstring(k.begin(), k.end()) + L".mp3.tmp")));
    REQUIRE(cache.put(key(2), blob(80, 2)));
    REQUIRE(cache.put(key(3), blob(80, 3)));
    CHECK(fs::exists(t.path() / foreign));
    CHECK(count_files(t.path()) == 4);
}
