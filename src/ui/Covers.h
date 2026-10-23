#pragma once
#include "lib/LibraryModel.h"

#include <windows.h>

#include <commctrl.h>

#include <memory>

namespace eb {

// Supplies the large-icon image list of the book grid. Real covers (T2.4) plug in here.
class CoverProvider {
public:
    CoverProvider() = default;
    CoverProvider(const CoverProvider&) = delete;
    CoverProvider& operator=(const CoverProvider&) = delete;
    virtual ~CoverProvider() = default;

    [[nodiscard]] virtual HIMAGELIST images() const noexcept = 0;
    [[nodiscard]] virtual int image_for(const BookRow& row) const noexcept = 0;
    virtual bool rebuild(UINT dpi) noexcept = 0;
};

// One drawn book icon per format. Returns null when the image list cannot be created.
std::unique_ptr<CoverProvider> make_generic_covers(UINT dpi) noexcept;

}
