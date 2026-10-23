#pragma once
#include <windows.h>

#include <utility>

namespace eb {

template <class Traits>
class Unique {
public:
    using Raw = typename Traits::Raw;

    Unique() noexcept = default;
    explicit Unique(Raw h) noexcept : h_(h) {}
    Unique(const Unique&) = delete;
    Unique& operator=(const Unique&) = delete;
    Unique(Unique&& o) noexcept : h_(std::exchange(o.h_, Raw{})) {}
    Unique& operator=(Unique&& o) noexcept
    {
        if (this != &o)
            reset(std::exchange(o.h_, Raw{}));
        return *this;
    }
    ~Unique() { reset(); }

    void reset(Raw h = Raw{}) noexcept
    {
        if (Traits::valid(h_))
            Traits::close(h_);
        h_ = h;
    }
    [[nodiscard]] Raw get() const noexcept { return h_; }
    [[nodiscard]] bool valid() const noexcept { return Traits::valid(h_); }
    explicit operator bool() const noexcept { return valid(); }

private:
    Raw h_{};
};

struct HandleTraits {
    using Raw = HANDLE;
    static bool valid(HANDLE h) noexcept { return h != nullptr && h != INVALID_HANDLE_VALUE; }
    static void close(HANDLE h) noexcept { CloseHandle(h); }
};

struct ModuleTraits {
    using Raw = HMODULE;
    static bool valid(HMODULE m) noexcept { return m != nullptr; }
    static void close(HMODULE m) noexcept { FreeLibrary(m); }
};

using UniqueHandle = Unique<HandleTraits>;
using UniqueModule = Unique<ModuleTraits>;

}
