#pragma once
#include "base/Handle.h"
#include "lib/LibraryModel.h"

#include <windows.h>

#include <commctrl.h>

namespace eb {

struct ImageListTraits {
    using Raw = HIMAGELIST;
    static bool valid(HIMAGELIST h) noexcept { return h != nullptr; }
    static void close(HIMAGELIST h) noexcept { ImageList_Destroy(h); }
};
using UniqueImageList = Unique<ImageListTraits>;

// Large-icon image list of the book grid: one drawn book icon per format. Real covers (T2.4) replace image_for().
class Covers {
public:
    [[nodiscard]] HIMAGELIST images() const noexcept { return list_.get(); }
    [[nodiscard]] static int image_for(const BookRow& row) noexcept;
    // Redraws for the dpi and the current system colours; a no-op when neither changed unless force is set.
    bool rebuild(UINT dpi, bool force = false) noexcept;

private:
    UniqueImageList list_;
    UINT dpi_ = 0;
};

}
