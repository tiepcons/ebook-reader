#include <doctest/doctest.h>

#include "base/DllLoader.h"

#include "test_base_tmp.h"

#include <filesystem>

TEST_CASE("load_checked accepts matching abi version")
{
    auto m = eb::load_checked(EB_MUPDF_DLL, "eb_abi_version", 3);
    CHECK(m.has_value());
}

TEST_CASE("load_checked rejects mismatch, missing export and relative path")
{
    auto bad = eb::load_checked(EB_MUPDF_DLL, "eb_abi_version", 4);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == eb::ErrorCode::Abi);
    auto none = eb::load_checked(EB_MUPDF_DLL, "no_such_export", 1);
    REQUIRE_FALSE(none.has_value());
    CHECK(none.error().code == eb::ErrorCode::Abi);
    CHECK_FALSE(eb::load_checked(std::filesystem::path(L"eb_mupdf.dll"), "eb_abi_version", 1).has_value());
}

TEST_CASE("load_checked on a missing absolute dll fails cleanly")
{
    const ebtest::TempDir t;
    auto r = eb::load_checked(t.path() / L"missing.dll", "eb_abi_version", 1);
    REQUIRE_FALSE(r.has_value());
    CHECK((r.error().code == eb::ErrorCode::NotFound || r.error().code == eb::ErrorCode::Io));
    CHECK_FALSE(eb::load_checked(EB_MUPDF_DLL, nullptr, 1).has_value());
}

TEST_CASE("secure_dll_search accepts absolute dir and rejects relative")
{
    const ebtest::TempDir t;
    CHECK(eb::secure_dll_search(t.path()).has_value());
    CHECK_FALSE(eb::secure_dll_search(std::filesystem::path(L"libs")).has_value());
}
