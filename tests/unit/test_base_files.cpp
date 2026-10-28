#include <doctest/doctest.h>

#include "base/AtomicFile.h"
#include "base/Hash.h"
#include "base/Paths.h"
#include "test_base_tmp.h"

#include <array>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}

TEST_CASE("write_atomic creates and then replaces the target")
{
    const ebtest::TempDir t;
    const fs::path f = t.path() / L"out.bin";
    const std::array<std::byte, 3> a{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    REQUIRE(eb::write_atomic(f, a).has_value());
    CHECK(slurp(f) == "abc");
    const std::array<std::byte, 1> b{std::byte{'z'}};
    REQUIRE(eb::write_atomic(f, b).has_value());
    CHECK(slurp(f) == "z");
}

TEST_CASE("write_atomic fails when the parent directory is missing")
{
    const ebtest::TempDir t;
    const std::array<std::byte, 1> a{std::byte{'a'}};
    CHECK_FALSE(eb::write_atomic(t.path() / L"nope" / L"out.bin", a).has_value());
}

TEST_CASE("book_id is stable for equal content and fails for a missing file")
{
    const ebtest::TempDir t;
    { std::ofstream(t.path() / L"a.txt", std::ios::binary) << "same content"; }
    { std::ofstream(t.path() / L"b.txt", std::ios::binary) << "same content"; }
    { std::ofstream(t.path() / L"c.txt", std::ios::binary) << "other content"; }
    const auto a = eb::book_id(t.path() / L"a.txt");
    const auto b = eb::book_id(t.path() / L"b.txt");
    const auto c = eb::book_id(t.path() / L"c.txt");
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(c.has_value());
    CHECK(*a == *b);
    CHECK(*a != *c);
    CHECK_FALSE(eb::book_id(t.path() / L"missing.txt").has_value());
}

TEST_CASE("dir_is_writable distinguishes real and missing directories")
{
    const ebtest::TempDir t;
    CHECK(eb::dir_is_writable(t.path()));
    CHECK_FALSE(eb::dir_is_writable(t.path() / L"missing"));
}
