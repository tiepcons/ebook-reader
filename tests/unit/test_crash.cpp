#include <doctest/doctest.h>

#include "base/CrashGuard.h"
#include "test_base_tmp.h"

#include <cstdlib>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

TEST_CASE("crash loop guard reports an unfinished operation")
{
    const ebtest::TempDir t;
    const fs::path marker = t.path() / L"suspect.txt";
    {
        eb::CrashLoopGuard g(marker);
        CHECK_FALSE(g.previous_suspect().has_value());
        REQUIRE(g.begin("open", "book-123").has_value());
    }
    const eb::CrashLoopGuard next(marker);
    REQUIRE(next.previous_suspect().has_value());
    CHECK(*next.previous_suspect() == "book-123");
}

TEST_CASE("crash loop guard is clear after end and scope")
{
    const ebtest::TempDir t;
    const fs::path marker = t.path() / L"suspect.txt";
    eb::CrashLoopGuard g(marker);
    REQUIRE(g.begin("open", "a").has_value());
    g.end();
    CHECK_FALSE(eb::CrashLoopGuard(marker).previous_suspect().has_value());
    {
        const eb::CrashLoopGuard::Scope s(g, "open", "b");
        CHECK(eb::CrashLoopGuard(marker).previous_suspect() == "b");
    }
    CHECK_FALSE(eb::CrashLoopGuard(marker).previous_suspect().has_value());
}

TEST_CASE("crash loop guard bounds its input")
{
    const ebtest::TempDir t;
    eb::CrashLoopGuard g(t.path() / L"suspect.txt");
    CHECK(g.begin("op", std::string(5000, 'x')).error().code == eb::ErrorCode::Limit);
}

namespace {

volatile bool g_stop = false; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

int recurse(int n) // NOLINT(misc-no-recursion): the overflow is the point of the test
{
    volatile char pad[512];
    pad[0] = static_cast<char>(n);
    if (g_stop)
        return 0;
    return recurse(n + 1) + pad[0];
}

void install_from_env()
{
    wchar_t dir[MAX_PATH];
    REQUIRE(GetEnvironmentVariableW(L"EB_CRASH_DIR", dir, MAX_PATH) > 0);
    eb::install_crash_handler(fs::path(dir));
}

}

TEST_CASE("crash child null" * doctest::skip())
{
    install_from_env();
    volatile int* p = nullptr;
    *p = 1;
}

TEST_CASE("crash child overflow" * doctest::skip())
{
    install_from_env();
    CHECK(recurse(0) == 0);
}

#ifndef __SANITIZE_ADDRESS__
namespace {

void run_child(const fs::path& dir, const wchar_t* test)
{
    REQUIRE(SetEnvironmentVariableW(L"EB_CRASH_DIR", dir.c_str()));
    wchar_t exe[MAX_PATH];
    REQUIRE(GetModuleFileNameW(nullptr, exe, MAX_PATH) > 0);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --no-skip -tc=\"" + test + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    REQUIRE(CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi));
    CHECK(WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

size_t count_fresh(const fs::path& dir, size_t& total)
{
    size_t fresh = 0;
    total = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        ++total;
        if (!e.path().filename().wstring().starts_with(L"eb-2000010"))
            ++fresh;
    }
    return fresh;
}

}

TEST_CASE("crash writes a minidump and prunes old ones")
{
    const ebtest::TempDir t;
    for (int i = 0; i < 7; ++i)
        std::ofstream(t.path() / (L"eb-2000010" + std::to_wstring(i) + L"-000000-1.dmp")) << "x";
    run_child(t.path(), L"crash child null");
    size_t total = 0;
    CHECK(count_fresh(t.path(), total) == 1);
    CHECK(total == 5);
}

TEST_CASE("stack overflow still writes a minidump")
{
    const ebtest::TempDir t;
    run_child(t.path(), L"crash child overflow");
    size_t total = 0;
    CHECK(count_fresh(t.path(), total) == 1);
    CHECK(total == 1);
}
#endif
