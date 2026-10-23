#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eb::scan_detail {

struct Entry {
    std::string path8;
    int64_t size = 0;
    int64_t mtime = 0;
};

// roots and holes are map-key prefixes (ordinal upper-case UTF-8, ending in a backslash).
// holes are subtrees that could not be fully enumerated.
struct WalkResult {
    std::vector<Entry> files;
    std::vector<std::string> roots;
    std::vector<std::string> holes;
    bool complete = true;
};

WalkResult walk_roots(std::span<const std::filesystem::path> roots, std::span<const std::wstring> exclude,
    const std::atomic<bool>& cancel);

std::wstring prefixed(const std::wstring& p);
std::string u8(std::wstring_view w);
std::wstring wide(std::string_view s);
std::wstring ext_of(std::wstring_view name);
std::string key_of(std::wstring_view w);
std::string dir_prefix(std::wstring_view dir);

}
