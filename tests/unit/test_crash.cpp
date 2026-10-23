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

TEST_CASE("crash child helper" * doctest::skip())
{
    wchar_t dir[MAX_PATH];
    REQUIRE(GetEnvironmentVariableW(L"EB_CRASH_DIR", dir, MAX_PATH) > 0);
    eb::install_crash_handler(fs::path(dir));
    volatile int* p = nullptr;
    *p = 1;
}

#ifndef __SANITIZE_ADDRESS__
TEST_CASE("crash writes a minidump and prunes old ones")
{
    const ebtest::TempDir t;
    for (int i = 0; i < 7; ++i)
        std::ofstream(t.path() / (L"eb-2000010" + std::to_wstring(i) + L"-000000-1.dmp")) << "x";
    REQUIRE(SetEnvironmentVariableW(L"EB_CRASH_DIR", t.path().c_str()));
    wchar_t exe[MAX_PATH];
    REQUIRE(GetModuleFileNameW(nullptr, exe, MAX_PATH) > 0);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --no-skip -tc=\"crash child helper\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    REQUIRE(CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi));
    CHECK(WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    size_t dumps = 0;
    size_t fresh = 0;
    for (const auto& e : fs::directory_iterator(t.path())) {
        ++dumps;
        if (!e.path().filename().wstring().starts_with(L"eb-2000010"))
            ++fresh;
    }
    CHECK(dumps == 5);
    CHECK(fresh == 1);
}
#endif
