#pragma once
#include "Result.h"

#include <windows.h>

#include <string>

namespace eb {

inline ErrorCode code_from_win32(DWORD e) noexcept
{
    switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return ErrorCode::NotFound;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
        return ErrorCode::Access;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return ErrorCode::NoMemory;
    default:
        return ErrorCode::Io;
    }
}

inline std::unexpected<Error> fail_win32(DWORD e, const wchar_t* what)
{
    return fail(code_from_win32(e), std::wstring(what) + L" (win32 " + std::to_wstring(e) + L")");
}

}
