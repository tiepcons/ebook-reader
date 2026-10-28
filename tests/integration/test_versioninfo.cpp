#include <doctest/doctest.h>

#include <windows.h>
#include <winver.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

struct Translation {
    WORD lang;
    WORD codepage;
};

std::wstring read_field(const wchar_t* path, const wchar_t* field)
{
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path, &ignored);
    if (size == 0)
        return {};
    std::vector<uint8_t> block(size);
    if (!GetFileVersionInfoW(path, 0, size, block.data()))
        return {};
    void* tr = nullptr;
    UINT tr_len = 0;
    if (!VerQueryValueW(block.data(), L"\\VarFileInfo\\Translation", &tr, &tr_len) || tr_len < sizeof(Translation))
        return {};
    const auto* t = static_cast<const Translation*>(tr);
    wchar_t key[96];
    swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\%s", t->lang, t->codepage, field);
    void* val = nullptr;
    UINT val_len = 0;
    if (!VerQueryValueW(block.data(), key, &val, &val_len) || val_len == 0)
        return {};
    return static_cast<const wchar_t*>(val);
}

}

TEST_CASE("exe and dll carry identical product name and version")
{
    const wchar_t* exe = EB_EXE_PATH;
    const wchar_t* dll = EB_MUPDF_DLL_W;
    CHECK(read_field(exe, L"ProductName") == L"Ebook Reader");
    CHECK(read_field(dll, L"ProductName") == L"Ebook Reader");
    const std::wstring ver = read_field(exe, L"ProductVersion");
    CHECK(!ver.empty());
    CHECK(read_field(dll, L"ProductVersion") == ver);
    CHECK(read_field(exe, L"OriginalFilename") == L"EbookReader.exe");
    CHECK(read_field(dll, L"OriginalFilename") == L"eb_mupdf.dll");
    CHECK(read_field(exe, L"CompanyName") == read_field(dll, L"CompanyName"));
    CHECK(read_field(exe, L"LegalCopyright") == L"AGPL-3.0-or-later, Ebook Reader contributors");
}
