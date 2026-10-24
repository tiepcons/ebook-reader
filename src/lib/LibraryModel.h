#pragma once
#include "Db.h"
#include "Scanner.h"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eb {

struct BookRow {
    std::string id;
    std::string title, author, fmt; // UTF-8; the UI converts lazily
    double percent = 0;
    int status = 0; // 0 ok, 1 missing, 2 bad (3 hidden is never returned)
};

enum class SortBy : unsigned char { Title, Author, Added, LastOpened };

struct Query {
    std::optional<std::wstring> folder; // the folder column or any subfolder of it
    std::wstring text;                  // accent-insensitive; every word must match title or author
    std::string fmt;
    SortBy sort = SortBy::Title;
    bool reading_only = false;
};

Result<std::vector<BookRow>> query_books(Db& db, const Query& q) noexcept;
Result<std::vector<std::wstring>> list_folders(Db& db) noexcept;
// The stored path with any internal marker suffix removed. NotFound when the id is unknown.
Result<std::wstring> book_path(Db& db, std::string_view id) noexcept;
// "Remove from library": hides the row (status 3) so rescans keep it away; never touches the file.
Result<void> hide_book(Db& db, std::string_view id) noexcept;
Result<void> unhide_all_books(Db& db) noexcept;

// A file named from outside the library: made absolute and canonical; it must be an existing regular file
// with a supported extension. NotFound otherwise.
Result<std::filesystem::path> resolve_external_book(std::wstring_view raw) noexcept;
// Id of the listed (status 0 or 2) row stored under this path, compared case-insensitively. NotFound when none.
Result<std::string> find_book_by_path(Db& db, const std::filesystem::path& file) noexcept;
// Makes the file an openable row (status 0, folder = parent directory) and returns its id. A row already stored
// under the path or with the same content id is revived instead of duplicated.
Result<std::string> upsert_external(Db& db, const std::filesystem::path& file, const BookMeta& meta) noexcept;

struct FolderNode {
    std::wstring label, prefix; // prefix is the folder string as stored in books.folder
    int parent = -1;            // index of the parent node, -1 for a root; parents precede children
};

// Folders are grouped under the root that contains them; a folder under no root becomes its own top node.
std::vector<FolderNode> build_folder_tree(std::span<const std::wstring> folders, std::span<const std::wstring> roots);

}
