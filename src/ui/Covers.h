#pragma once
#include "base/Handle.h"
#include "lib/LibraryModel.h"
#include "lib/SlotLru.h"

#include <windows.h>

#include <commctrl.h>

#include <cstdint>
#include <string>
#include <vector>

namespace eb {

struct ImageListTraits {
    using Raw = HIMAGELIST;
    static bool valid(HIMAGELIST h) noexcept { return h != nullptr; }
    static void close(HIMAGELIST h) noexcept { ImageList_Destroy(h); }
};
using UniqueImageList = Unique<ImageListTraits>;

// Large-icon image list of the book grid. Slots 0..7 are drawn per-format placeholders; the rest hold real
// covers (a byte-budgeted LRU that never evicts what the current paint used).
class Covers {
public:
    static constexpr size_t kBudgetBytes = size_t{12} << 20;
    static constexpr size_t kMinCovers = 64, kMaxCovers = 256;

    Covers() = default;
    Covers(const Covers&) = delete;
    Covers& operator=(const Covers&) = delete;

    [[nodiscard]] HIMAGELIST images() const noexcept { return list_.get(); }
    [[nodiscard]] SIZE cell() const noexcept { return {cell_w_, cell_h_}; }
    // The book's cover slot when loaded (and marks it used in this paint), else its format placeholder.
    [[nodiscard]] int image_for(const BookRow& row);
    [[nodiscard]] bool has(const std::string& id) const { return lru_.contains(id); }
    // False while every slot was used by the current paint: asking for more covers would only thrash.
    [[nodiscard]] bool can_accept() const noexcept { return !lru_.saturated(); }
    void begin_paint() noexcept { lru_.begin_generation(); }
    // pixels: premultiplied BGRA exactly cell() in size.
    bool set_cover(const std::string& id, const std::vector<uint32_t>& pixels);
    // Redraws the placeholders for the dpi and drops every loaded cover; a no-op when the dpi is unchanged and
    // force is not set.
    bool rebuild(UINT dpi, bool force = false);

private:
    UniqueImageList list_;
    UINT dpi_ = 0;
    int cell_w_ = 0, cell_h_ = 0;
    SlotLru lru_;
};

}
