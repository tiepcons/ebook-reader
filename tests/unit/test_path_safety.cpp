#include <doctest/doctest.h>

#include "base/PathSafety.h"

TEST_CASE("is_safe_book_path rejects device namespaces")
{
    using eb::is_safe_book_path;
    CHECK(is_safe_book_path(L"C:\\books\\a.epub"));
    CHECK(is_safe_book_path(L"\\\\server\\share\\a.epub"));
    CHECK(is_safe_book_path(L"\\\\?\\C:\\books\\a.epub"));
    CHECK(is_safe_book_path(L"\\\\?\\UNC\\server\\share\\a.epub"));
    CHECK(is_safe_book_path(L"relative\\a.epub"));
    CHECK_FALSE(is_safe_book_path(L""));
    CHECK_FALSE(is_safe_book_path(L"\\\\.\\PhysicalDrive0"));
    CHECK_FALSE(is_safe_book_path(L"//./pipe/x"));
    CHECK_FALSE(is_safe_book_path(L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolume1\\a.epub"));
    CHECK_FALSE(is_safe_book_path(L"\\\\?\\Volume{1234}\\a.epub"));
}

TEST_CASE("is_safe_book_path rejects reserved DOS names")
{
    using eb::is_safe_book_path;
    CHECK_FALSE(is_safe_book_path(L"NUL"));
    CHECK_FALSE(is_safe_book_path(L"C:\\books\\con.txt"));
    CHECK_FALSE(is_safe_book_path(L"C:\\books\\Aux.epub"));
    CHECK_FALSE(is_safe_book_path(L"C:\\books\\COM1"));
    CHECK_FALSE(is_safe_book_path(L"C:\\books\\lpt9.txt"));
    CHECK_FALSE(is_safe_book_path(L"C:\\books\\NUL :stream"));
    CHECK_FALSE(is_safe_book_path(L"C:NUL"));
    CHECK(is_safe_book_path(L"C:\\books\\console.txt"));
    CHECK(is_safe_book_path(L"C:\\books\\COM0.txt"));
    CHECK(is_safe_book_path(L"C:\\books\\COM10.txt"));
}
