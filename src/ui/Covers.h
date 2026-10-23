#pragma once
#include "base/Handle.h"
#include "lib/LibraryModel.h"

#include <windows.h>

#include <commctrl.h>

#include <filesystem>
#include <list>
#include <string>
#include <unordered_map>

namespace eb {

struct ImageListTraits {
    using Raw = HIMAGELIST;
    static bool valid(HIMAGELIST h) noexcept { return h != nullptr; }
    static void close(HIMAGELIST h) noexcept { ImageList_Destroy(h); }
};
using UniqueImageList = Unique<ImageListTraits>;

// Large-icon image list of the book grid. Slots 0..7 are drawn per-format placeholders; the rest hold real
// covers in an LRU of kMaxCovers entries.
class Covers {
public:
    static constexpr size_t kMaxCovers = 256;

    Covers() = default;
    Covers(const Covers&) = delete;
    Covers& operator=(const Covers&) = delete;

    [[nodiscard]] HIMAGELIST images() const noexcept { return list_.get(); }
    // The book's cover slot when loaded (and marks it recently used), else its format placeholder.
    [[nodiscard]] int image_for(const BookRow& row);
    [[nodiscard]] bool has(const std::string& id) const { return map_.contains(id); }
    // Decodes the JPEG, letterboxes it on a neutral background and stores it under id.
    bool set_cover(const std::string& id, const std::filesystem::path& jpeg);
    // Redraws the placeholders for the dpi and drops every loaded cover; a no-op when the dpi is unchanged and
    // force is not set.
    bool rebuild(UINT dpi, bool force = false);

private:
    struct Entry {
        std::string id;
        int slot;
    };

    UniqueImageList list_;
    UINT dpi_ = 0;
    int cell_w_ = 0, cell_h_ = 0;
    std::list<Entry> lru_; // front = most recently used
    std::unordered_map<std::string, std::list<Entry>::iterator> map_;
};

}
