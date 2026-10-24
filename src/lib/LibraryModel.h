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

struct ExternalFile {
    std::filesystem::path path; // absolute, canonical
    std::string id, path8, folder;
    int64_t size = 0, mtime = 0;
    bool copy_alive = false; // a listed row with this content id still has its file on disk
};

// All the file I/O for a book named from outside the library (resolve, safety check, hashing, attributes);
// meant for a worker thread. db is only read. NotFound unless it is an existing regular file with a supported
// extension that is not a device path.
Result<ExternalFile> inspect_external_book(Db& db, std::wstring_view raw) noexcept;
// Database work only. Makes the file an openable row (status 0, folder = parent directory) and returns its id.
// A row under the same path is revived; a row with the same content id is revived as is when its own file still
// exists, and moved to this path otherwise.
Result<std::string> upsert_external(Db& db, const ExternalFile& file, const BookMeta& meta) noexcept;

struct FolderNode {
    std::wstring label, prefix; // prefix is the folder string as stored in books.folder
    int parent = -1;            // index of the parent node, -1 for a root; parents precede children
};

// Folders are grouped under the root that contains them; a folder under no root becomes its own top node.
std::vector<FolderNode> build_folder_tree(std::span<const std::wstring> folders, std::span<const std::wstring> roots);

}
