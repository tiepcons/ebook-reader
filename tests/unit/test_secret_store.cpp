#include <doctest/doctest.h>

#include "base/SecretStore.h"

#include <windows.h>
#include <wincred.h>

#include <string>

namespace {

constexpr const wchar_t* kTestPrefix = L"EbookReader.tiepcons/Test/";

void remove_leftovers()
{
    DWORD count = 0;
    CREDENTIALW** list = nullptr;
    if (!CredEnumerateW((std::wstring(kTestPrefix) + L"*").c_str(), 0, &count, &list))
        return;
    for (DWORD i = 0; i < count; ++i)
        CredDeleteW(list[i]->TargetName, list[i]->Type, 0);
    CredFree(static_cast<void*>(list));
}

eb::SecretString make(std::wstring_view s)
{
    auto r = eb::SecretString::copy_of(s);
    REQUIRE(r.has_value());
    return std::move(*r);
}

std::wstring unique_target()
{
    static int counter = 0;
    return std::wstring(kTestPrefix) + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()) + L"_"
        + std::to_wstring(++counter);
}

struct CredGuard {
    std::wstring target = unique_target();
    CredGuard() { remove_leftovers(); }
    CredGuard(const CredGuard&) = delete;
    CredGuard& operator=(const CredGuard&) = delete;
    ~CredGuard() { [[maybe_unused]] const auto r = eb::secret_delete(target); }
};

}

TEST_CASE("secret round-trip, overwrite and delete")
{
    const CredGuard g;
    REQUIRE(eb::secret_set(g.target, make(L"kéy-123")).has_value());
    auto got = eb::secret_get(g.target);
    REQUIRE(got.has_value());
    CHECK((got->view() == L"kéy-123"));

    REQUIRE(eb::secret_set(g.target, make(L"second")).has_value());
    CHECK((eb::secret_get(g.target)->view() == L"second"));

    REQUIRE(eb::secret_delete(g.target).has_value());
    auto gone = eb::secret_get(g.target);
    REQUIRE_FALSE(gone.has_value());
    CHECK(gone.error().code == eb::ErrorCode::NotFound);
    CHECK(eb::secret_delete(g.target).error().code == eb::ErrorCode::NotFound);
}

TEST_CASE("secret limits")
{
    const CredGuard g;
    CHECK(eb::secret_set(g.target, make(std::wstring(eb::kMaxSecretChars, L'a'))).has_value());
    CHECK((eb::secret_get(g.target)->view().size() == eb::kMaxSecretChars));
    CHECK_FALSE(eb::SecretString::copy_of(std::wstring(eb::kMaxSecretChars + 1, L'a')).has_value());
    CHECK_FALSE(eb::secret_set(g.target, eb::SecretString{}).has_value());
    CHECK_FALSE(eb::secret_set(L"", make(L"x")).has_value());
    CHECK_FALSE(eb::secret_get(std::wstring(300, L't')).has_value());
}

TEST_CASE("api key validation")
{
    CHECK(eb::validate_api_key(make(std::wstring(32, L'a'))).has_value());
    CHECK(eb::validate_api_key(make(std::wstring(128, L'Z'))).has_value());
    CHECK_FALSE(eb::validate_api_key(make(std::wstring(31, L'a'))).has_value());
    CHECK_FALSE(eb::validate_api_key(make(std::wstring(129, L'a'))).has_value());
    CHECK_FALSE(eb::validate_api_key(make(std::wstring(31, L'a') + L"\n")).has_value());
    CHECK_FALSE(eb::validate_api_key(make(std::wstring(31, L'a') + L" ")).has_value());
    CHECK_FALSE(eb::validate_api_key(make(std::wstring(31, L'a') + L"é")).has_value());
    const auto bad = eb::validate_api_key(make(L"short"));
    CHECK((bad.error().detail == L"Khóa API không hợp lệ (chỉ gồm chữ và số)"));
}

TEST_CASE("SecretString from_window_text reads an edit control")
{
    HWND edit = CreateWindowExW(0, L"EDIT", L"abc123", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, nullptr, nullptr);
    REQUIRE(edit != nullptr);
    const eb::SecretString s = eb::SecretString::from_window_text(edit);
    CHECK((s.view() == L"abc123"));
    DestroyWindow(edit);
}

TEST_CASE("SecretString move leaves source empty")
{
    auto a = eb::SecretString::copy_of(L"abc");
    REQUIRE(a.has_value());
    eb::SecretString b(std::move(*a));
    CHECK((b.view() == L"abc"));
    CHECK(a->view().empty());
    eb::SecretString c;
    c = std::move(b);
    CHECK((c.view() == L"abc"));
    CHECK(b.view().empty());
}
