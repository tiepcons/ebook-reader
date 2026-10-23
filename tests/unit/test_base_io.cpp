#include <doctest/doctest.h>

#include "base/AtomicFile.h"
#include "base/Hash.h"
#include "base/Log.h"
#include "base/Paths.h"
#include "test_base_tmp.h"

#include <aclapi.h>

#include <cstddef>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
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

TEST_CASE("log keeps the size bound when rotation cannot move the file")
{
    const ebtest::TempDir t;
    const fs::path log = t.path() / L"app.log";
    { const std::ofstream touch(log); }
    const HANDLE holder = CreateFileW(log.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(holder != INVALID_HANDLE_VALUE);
    eb::log::init(log);
    const std::string line(1000, 'x');
    for (int i = 0; i < 2500; ++i)
        eb::log::info("line {} {}", i, line);
    eb::log::shutdown();
    CloseHandle(holder);
    CHECK(size_of(log) <= 1024 * 1024);
    CHECK_FALSE(fs::exists(t.path() / L"app.1.log"));
}

TEST_CASE("log is thread safe and lines are never torn")
{
    const ebtest::TempDir t;
    eb::log::init(t.path() / L"app.log");
    std::vector<std::thread> threads;
    threads.reserve(4);
    for (int k = 0; k < 4; ++k) {
        threads.emplace_back([k] {
            for (int i = 0; i < 2000; ++i)
                eb::log::info("worker {} item {} payload-0123456789 END", k, i);
        });
    }
    for (auto& th : threads)
        th.join();
    eb::log::shutdown();
    size_t lines = 0;
    for (const wchar_t* name : {L"app.log", L"app.1.log"}) {
        std::ifstream in(t.path() / name);
        std::string l;
        while (std::getline(in, l)) {
            ++lines;
            CHECK(l.find("[INFO] worker ") != std::string::npos);
            CHECK(l.ends_with(" payload-0123456789 END"));
        }
    }
    CHECK(lines == 8000);
}

namespace {

bool deny_add_file(const fs::path& dir)
{
    PACL old_dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    std::wstring name = dir.wstring();
    if (GetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &old_dacl,
            nullptr, &sd) != ERROR_SUCCESS)
        return false;
    EXPLICIT_ACCESSW ea{};
    std::wstring who = L"CURRENT_USER";
    ea.grfAccessPermissions = FILE_ADD_FILE;
    ea.grfAccessMode = DENY_ACCESS;
    ea.grfInheritance = NO_INHERITANCE;
    BuildTrusteeWithNameW(&ea.Trustee, who.data());
    PACL new_dacl = nullptr;
    bool ok = SetEntriesInAclW(1, &ea, old_dacl, &new_dacl) == ERROR_SUCCESS;
    if (ok)
        ok = SetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                 new_dacl, nullptr) == ERROR_SUCCESS;
    LocalFree(new_dacl);
    LocalFree(sd);
    return ok;
}

}

TEST_CASE("dir_is_writable reflects real permissions")
{
    const ebtest::TempDir t;
    CHECK(eb::dir_is_writable(t.path()));
    CHECK_FALSE(eb::dir_is_writable(t.path() / L"nope"));
    const fs::path locked = t.path() / L"locked";
    fs::create_directories(locked);
    REQUIRE(deny_add_file(locked));
    CHECK_FALSE(eb::dir_is_writable(locked));
}
