#pragma once
#include "Handle.h"
#include "Result.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace eb {

// Minimal Win32 file reading; replaces <fstream>, which drags the whole iostreams/locale runtime into the exe.
class FileReader {
public:
    explicit FileReader(const std::filesystem::path& p) noexcept;
    [[nodiscard]] bool ok() const noexcept { return h_.valid(); }
    [[nodiscard]] bool size(uint64_t& out) const noexcept;
    // false on an I/O error; got == 0 with true means end of file.
    [[nodiscard]] bool read(void* buf, size_t want, size_t& got) noexcept;

private:
    UniqueHandle h_;
};

// First min(file size, max) bytes; empty when the file cannot be opened.
[[nodiscard]] std::string read_head(const std::filesystem::path& p, size_t max);

// Whole file; Limit when larger than max, NotFound/Io/Access when unreadable.
[[nodiscard]] Result<std::string> read_all(const std::filesystem::path& p, size_t max);

// Same as read_all, as raw bytes (no string copy).
[[nodiscard]] Result<std::vector<std::byte>> read_all_bytes(const std::filesystem::path& p, size_t max);

// Create or truncate, then write; true when every byte was written.
[[nodiscard]] bool write_file(const std::filesystem::path& p, std::string_view data) noexcept;

// Create a new file (never follows or overwrites an existing entry, reparse points included); false if it exists.
[[nodiscard]] bool write_new(const std::filesystem::path& p, std::string_view data) noexcept;

}
