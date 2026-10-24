#pragma once
#include "Result.h"

#include <cstddef>
#include <memory>
#include <string_view>

struct HWND__;

namespace eb {

inline constexpr size_t kMaxSecretChars = 512;
inline constexpr size_t kMinApiKeyChars = 32;
inline constexpr size_t kMaxApiKeyChars = 128;
inline constexpr std::wstring_view kAzureSecretTarget = L"EbookReader.tiepcons/AzureSpeechKey/v1";

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
    // Reads an edit control straight into the zeroed buffer; empty when unreadable or over kMaxSecretChars.
    static SecretString from_window_text(HWND__* edit) noexcept;

    [[nodiscard]] std::wstring_view view() const noexcept { return {buf_.get(), size_}; }

private:
    void wipe() noexcept;

    std::unique_ptr<wchar_t[]> buf_;
    size_t size_ = 0;
};

// Printable ASCII 0x21-0x7E, kMinApiKeyChars..kMaxApiKeyChars; error detail is the user-facing message.
Result<void> validate_api_key(const SecretString& key) noexcept;
Result<void> secret_set(std::wstring_view target, const SecretString& secret) noexcept;
Result<SecretString> secret_get(std::wstring_view target) noexcept;
Result<void> secret_delete(std::wstring_view target) noexcept;

}
