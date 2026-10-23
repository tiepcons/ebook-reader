#include "AtomicFile.h"

#include "Handle.h"
#include "WinError.h"

#include <algorithm>
#include <new>

namespace eb {
namespace {

constexpr size_t kChunk = 1u << 20;

Result<void> write_all(HANDLE h, std::span<const std::byte> data)
{
    while (!data.empty()) {
        const DWORD want = static_cast<DWORD>(std::min(data.size(), kChunk));
        DWORD got = 0;
        if (!WriteFile(h, data.data(), want, &got, nullptr))
            return fail_win32(GetLastError(), L"WriteFile");
        data = data.subspan(got);
    }
    return {};
}

Result<void> write_temp(const std::filesystem::path& tmp, std::span<const std::byte> data)
{
    const UniqueHandle h(
        CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!h)
        return fail_win32(GetLastError(), L"CreateFile temp");
    if (auto r = write_all(h.get(), data); !r)
        return r;
    if (!FlushFileBuffers(h.get()))
        return fail_win32(GetLastError(), L"FlushFileBuffers");
    return {};
}

}

Result<void> write_atomic(const std::filesystem::path& target, std::span<const std::byte> data)
{
    try {
        std::filesystem::path tmp = target;
        tmp += L".tmp" + std::to_wstring(GetCurrentThreadId());
        auto r = write_temp(tmp, data);
        if (r && !MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            r = fail_win32(GetLastError(), L"MoveFileEx");
        if (!r)
            DeleteFileW(tmp.c_str());
        return r;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::NoMemory);
    } catch (...) {
        return fail(ErrorCode::Internal);
    }
}

}
