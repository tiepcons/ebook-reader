#include <doctest/doctest.h>

#include "base/AtomicFile.h"
#include "base/Hash.h"
#include "base/Log.h"
#include "base/Paths.h"
#include "test_base_tmp.h"

#include <cstddef>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::vector<std::byte> bytes(size_t n, unsigned char v)
{
    return std::vector<std::byte>(n, static_cast<std::byte>(v));
}

void put(const fs::path& p, const std::vector<std::byte>& d)
{
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
}

constexpr size_t kTwoSlices = 131072;

size_t size_of(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    return static_cast<size_t>(f.tellg());
}

size_t count_entries(const fs::path& dir)
{
    return static_cast<size_t>(std::distance(fs::directory_iterator(dir), fs::directory_iterator()));
}

bool never_writable(const fs::path&) noexcept
{
    return false;
}

}

TEST_CASE("resolve_paths portable when exe dir is writable")
{
    const ebtest::TempDir t;
    auto r = eb::resolve_paths(t.path());
    REQUIRE(r.has_value());
    CHECK(r->data == t.path() / L"data");
    CHECK(fs::is_directory(r->covers));
    CHECK(fs::is_directory(r->ebooks));
    CHECK(fs::is_directory(r->libs));
    CHECK(fs::is_directory(r->fonts));
    CHECK(count_entries(t.path() / L"data") == 5);
}

TEST_CASE("resolve_paths falls back to local root when exe dir is read-only")
{
    const ebtest::TempDir exe;
    const ebtest::TempDir local;
    auto r = eb::resolve_paths(exe.path(), local.path() / L"EbookReader", &never_writable);
    REQUIRE(r.has_value());
    CHECK(r->data == local.path() / L"EbookReader");
    CHECK(fs::is_directory(r->logs));
    CHECK(r->ebooks == exe.path() / L"ebooks");
    CHECK_FALSE(fs::exists(exe.path() / L"data"));
}

TEST_CASE("write_atomic replaces content and leaves no temp file")
{
    const ebtest::TempDir t;
    const fs::path f = t.path() / L"state.bin";
    REQUIRE(eb::write_atomic(f, bytes(10, 1)).has_value());
    REQUIRE(eb::write_atomic(f, bytes(3, 2)).has_value());
    CHECK(size_of(f) == 3);
    CHECK(count_entries(t.path()) == 1);
    CHECK(eb::write_atomic(t.path() / L"missing" / L"x.bin", bytes(1, 0)).error().code == eb::ErrorCode::NotFound);
}

TEST_CASE("book_id is stable across rename and sensitive to content")
{
    const ebtest::TempDir t;
    for (const size_t n : {size_t{0}, size_t{100}, size_t{65536}, size_t{70000}, size_t{200000}}) {
        const fs::path a = t.path() / L"a.bin";
        const fs::path b = t.path() / L"b.bin";
        auto d = bytes(n, 7);
        put(a, d);
        auto id1 = eb::book_id(a);
        REQUIRE(id1.has_value());
        CHECK(id1->size() == 64);
        fs::rename(a, b);
        CHECK(*eb::book_id(b) == *id1);
        if (n > 0) {
            d[n / 2] = std::byte{8};
            put(b, d);
            const bool middle_hashed = n <= kTwoSlices;
            CHECK((*eb::book_id(b) != *id1) == middle_hashed);
            d[n / 2] = std::byte{7};
            d[0] = std::byte{9};
            put(b, d);
            CHECK(*eb::book_id(b) != *id1);
        }
        fs::remove(b);
    }
    CHECK(eb::book_id(t.path() / L"none").error().code == eb::ErrorCode::NotFound);
}

TEST_CASE("log rotates at 1 MB keeping two files")
{
    const ebtest::TempDir t;
    eb::log::init(t.path() / L"app.log");
    const std::string line(1000, 'x');
    for (int i = 0; i < 1500; ++i)
        eb::log::info("line {} {}", i, line);
    eb::log::error("done {}", 1);
    eb::log::shutdown();
    CHECK(fs::exists(t.path() / L"app.1.log"));
    CHECK(size_of(t.path() / L"app.log") <= 1024 * 1024);
    CHECK(size_of(t.path() / L"app.1.log") <= 1024 * 1024);
    CHECK(count_entries(t.path()) == 2);
}
