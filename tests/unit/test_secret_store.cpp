#include <doctest/doctest.h>

#include "base/SecretStore.h"

#include <windows.h>

#include <string>

namespace {

std::wstring unique_target()
{
    static int counter = 0;
    return L"EbookReader/Test/" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()) + L"_"
        + std::to_wstring(++counter);
}

struct CredGuard {
    std::wstring target = unique_target();
    CredGuard() = default;
    CredGuard(const CredGuard&) = delete;
    CredGuard& operator=(const CredGuard&) = delete;
    ~CredGuard() { (void)eb::secret_delete(target); }
};

}

TEST_CASE("secret round-trip, overwrite and delete")
{
    const CredGuard g;
    REQUIRE(eb::secret_set(g.target, L"kéy-123").has_value());
    auto got = eb::secret_get(g.target);
    REQUIRE(got.has_value());
    CHECK(got->view() == L"kéy-123");

    REQUIRE(eb::secret_set(g.target, L"second").has_value());
    CHECK(eb::secret_get(g.target)->view() == L"second");

    REQUIRE(eb::secret_delete(g.target).has_value());
    auto gone = eb::secret_get(g.target);
    REQUIRE_FALSE(gone.has_value());
    CHECK(gone.error().code == eb::ErrorCode::NotFound);
    CHECK(eb::secret_delete(g.target).error().code == eb::ErrorCode::NotFound);
}

TEST_CASE("secret limits")
{
    const CredGuard g;
    CHECK(eb::secret_set(g.target, std::wstring(eb::kMaxSecretChars, L'a')).has_value());
    CHECK(eb::secret_get(g.target)->view().size() == eb::kMaxSecretChars);
    CHECK_FALSE(eb::secret_set(g.target, std::wstring(eb::kMaxSecretChars + 1, L'a')).has_value());
    CHECK_FALSE(eb::secret_set(g.target, L"").has_value());
    CHECK_FALSE(eb::secret_set(L"", L"x").has_value());
    CHECK_FALSE(eb::secret_get(std::wstring(300, L't')).has_value());
}

TEST_CASE("SecretString move leaves source empty")
{
    auto a = eb::SecretString::copy_of(L"abc");
    REQUIRE(a.has_value());
    eb::SecretString b(std::move(*a));
    CHECK(b.view() == L"abc");
    CHECK(a->view().empty());
    eb::SecretString c;
    c = std::move(b);
    CHECK(c.view() == L"abc");
    CHECK(b.view().empty());
}
