#include <doctest/doctest.h>

#include "base/DllLoader.h"

#include <filesystem>

TEST_CASE("load_checked accepts matching abi version")
{
    auto m = eb::load_checked(EB_MUPDF_DLL, "eb_abi_version", 1);
    CHECK(m.has_value());
}

TEST_CASE("load_checked rejects mismatch, missing export and relative path")
{
    auto bad = eb::load_checked(EB_MUPDF_DLL, "eb_abi_version", 2);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == eb::ErrorCode::Abi);
    auto none = eb::load_checked(EB_MUPDF_DLL, "no_such_export", 1);
    REQUIRE_FALSE(none.has_value());
    CHECK(none.error().code == eb::ErrorCode::Abi);
    CHECK_FALSE(eb::load_checked(std::filesystem::path(L"eb_mupdf.dll"), "eb_abi_version", 1).has_value());
}
