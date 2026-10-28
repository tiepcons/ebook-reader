#pragma once
#include <expected>
#include <string>
#include <utility>

namespace eb {

enum class ErrorCode : unsigned char { Io, NotFound, Access, Format, Limit, NoMemory, Abi, Internal, Unavailable };

struct Error {
    ErrorCode code{};
    std::wstring detail;
};

template <class T>
using Result = std::expected<T, Error>;

inline std::unexpected<Error> fail(ErrorCode code, std::wstring detail = {})
{
    return std::unexpected<Error>(Error{code, std::move(detail)});
}

}
