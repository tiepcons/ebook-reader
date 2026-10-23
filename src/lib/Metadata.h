#pragma once
#include "Scanner.h"
#include "eb_mupdf.h"

#include <filesystem>
#include <memory>

namespace eb {

struct DocCloser {
    void operator()(eb_doc* d) const noexcept { eb_close(d); }
};
using UniqueDoc = std::unique_ptr<eb_doc, DocCloser>;

ErrorCode error_from_status(eb_status s) noexcept;

// Opens any supported book; .txt is converted to FB2 in memory first. Reflowable documents are laid out
// for the metadata/cover pass. Needs eb_init to have been called.
Result<UniqueDoc> open_book(const std::filesystem::path& file) noexcept;

// MetaFn implementation backed by the engine. Safe to call on a worker thread.
Result<BookMeta> read_book_meta(const std::filesystem::path& file) noexcept;

}
