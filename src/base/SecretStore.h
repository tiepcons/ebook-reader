#pragma once
#include "Result.h"

#include <cstddef>
#include <memory>
#include <string_view>

namespace eb {

inline constexpr size_t kMaxSecretChars = 512;
inline constexpr std::wstring_view kAzureSecretTarget = L"EbookReader/AzureSpeech";

// Owns a wide-char buffer that is zeroed before release. Never log its contents.
class SecretString {
public:
    SecretString() noexcept = default;
    SecretString(SecretString&& o) noexcept;
    SecretString& operator=(SecretString&& o) noexcept;
    SecretString(const SecretString&) = delete;
    SecretString& operator=(const SecretString&) = delete;
    ~SecretString();

    static Result<SecretString> copy_of(std::wstring_view s) noexcept;

    [[nodiscard]] std::wstring_view view() const noexcept { return {buf_.get(), size_}; }

private:
    void wipe() noexcept;

    std::unique_ptr<wchar_t[]> buf_;
    size_t size_ = 0;
};

Result<void> secret_set(std::wstring_view target, std::wstring_view secret) noexcept;
Result<SecretString> secret_get(std::wstring_view target) noexcept;
Result<void> secret_delete(std::wstring_view target) noexcept;

}
