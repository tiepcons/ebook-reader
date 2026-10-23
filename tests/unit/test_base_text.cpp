#include <doctest/doctest.h>

#include "base/Text.h"

#include <string>

TEST_CASE("utf8 round trip")
{
    const std::wstring w = L"Truy\u1ec7n Ki\u1ec1u \U0001F600";
    auto u = eb::wide_to_utf8(w);
    REQUIRE(u.has_value());
    auto back = eb::utf8_to_wide(*u);
    REQUIRE(back.has_value());
    CHECK(*back == w);
    CHECK(eb::utf8_to_wide("")->empty());
}

TEST_CASE("invalid utf8 and utf16 rejected")
{
    auto r = eb::utf8_to_wide(std::string("ab\xC3\x28"));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == eb::ErrorCode::Format);
    const std::wstring lone = std::wstring(1, static_cast<wchar_t>(0xD800));
    CHECK_FALSE(eb::wide_to_utf8(lone).has_value());
}

TEST_CASE("fold_for_search strips Vietnamese diacritics")
{
    CHECK(eb::fold_for_search(L"Truy\u1ec7n Ki\u1ec1u \u2013 \u0110\u00c0 L\u1ea0T") == L"truyen kieu \u2013 da lat");
    CHECK(eb::fold_for_search(L"").empty());
}

TEST_CASE("fold_for_search tolerates lone surrogates")
{
    std::wstring in = L"Tiếng ";
    in.push_back(static_cast<wchar_t>(0xD800));
    in += L" Việt";
    const std::wstring out = eb::fold_for_search(in);
    CHECK(out.starts_with(L"tieng "));
    CHECK(out.ends_with(L" viet"));
}
