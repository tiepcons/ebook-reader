#include "SecretStore.h"

#include <windows.h>
// wincred.h needs windows.h first
#include <wincred.h>

#include <algorithm>
#include <new>
#include <string>
#include <utility>

namespace eb {
namespace {

constexpr size_t kMaxTargetChars = 256;

struct CredDeleter {
    void operator()(CREDENTIALW* c) const noexcept
    {
        if (c->CredentialBlob != nullptr && c->CredentialBlobSize != 0)
            SecureZeroMemory(c->CredentialBlob, c->CredentialBlobSize);
        CredFree(c);
    }
};
using CredPtr = std::unique_ptr<CREDENTIALW, CredDeleter>;

Result<std::wstring> check_target(std::wstring_view target)
{
    if (target.empty() || target.size() > kMaxTargetChars || target.find(L'\0') != std::wstring_view::npos)
        return fail(ErrorCode::Limit, L"bad credential target");
    return std::wstring(target);
}

std::unexpected<Error> win_fail(DWORD err) noexcept
{
    if (err == ERROR_NOT_FOUND)
        return fail(ErrorCode::NotFound);
    if (err == ERROR_ACCESS_DENIED)
        return fail(ErrorCode::Access);
    if (err == ERROR_NO_SUCH_LOGON_SESSION || err == ERROR_NOT_SUPPORTED)
        return fail(ErrorCode::Unavailable, L"Credential Manager không khả dụng trên máy này");
    return fail(ErrorCode::Io);
}

}

SecretString::SecretString(SecretString&& o) noexcept : buf_(std::move(o.buf_)), size_(std::exchange(o.size_, 0)) {}

SecretString& SecretString::operator=(SecretString&& o) noexcept
{
    if (this != &o) {
        wipe();
        buf_ = std::move(o.buf_);
        size_ = std::exchange(o.size_, 0);
    }
    return *this;
}

SecretString::~SecretString()
{
    wipe();
}

void SecretString::wipe() noexcept
{
    if (buf_)
        SecureZeroMemory(buf_.get(), size_ * sizeof(wchar_t));
    buf_.reset();
    size_ = 0;
}

Result<SecretString> SecretString::copy_of(std::wstring_view s) noexcept
{
    if (s.size() > kMaxSecretChars)
        return fail(ErrorCode::Limit, L"secret too long");
    SecretString out;
    out.buf_.reset(new (std::nothrow) wchar_t[s.size() + 1]);
    if (!out.buf_)
        return fail(ErrorCode::NoMemory);
    out.size_ = s.size();
    for (size_t i = 0; i < s.size(); ++i)
        out.buf_[i] = s[i];
    out.buf_[s.size()] = L'\0';
    return out;
}

SecretString SecretString::from_window_text(HWND__* edit) noexcept
{
    SecretString out;
    const int len = GetWindowTextLengthW(edit);
    if (len <= 0 || static_cast<size_t>(len) > kMaxSecretChars)
        return out;
    out.buf_.reset(new (std::nothrow) wchar_t[static_cast<size_t>(len) + 1]);
    if (!out.buf_)
        return out;
    out.size_ = static_cast<size_t>(len);
    const int got = GetWindowTextW(edit, out.buf_.get(), len + 1);
    SecureZeroMemory(out.buf_.get() + got, (static_cast<size_t>(len) + 1 - static_cast<size_t>(got)) * sizeof(wchar_t));
    out.size_ = static_cast<size_t>(got);
    return out;
}

Result<void> validate_api_key(const SecretString& key) noexcept
{
    const std::wstring_view v = key.view();
    const bool printable = std::all_of(v.begin(), v.end(), [](wchar_t c) { return c >= 0x21 && c <= 0x7E; });
    if (!printable || v.size() < kMinApiKeyChars || v.size() > kMaxApiKeyChars)
        return fail(ErrorCode::Format, L"Khóa API không hợp lệ (chỉ gồm chữ và số)");
    return {};
}

Result<void> secret_set(std::wstring_view target, const SecretString& secret) noexcept
{
    try {
        auto name = check_target(target);
        if (!name)
            return std::unexpected(std::move(name.error()));
        if (secret.view().empty())
            return fail(ErrorCode::Format, L"empty secret");
        wchar_t comment[] = L"Ebook Reader – Azure Speech key";
        wchar_t user[] = L"azure-speech";
        CREDENTIALW cred{};
        cred.Type = CRED_TYPE_GENERIC;
        cred.TargetName = name->data();
        cred.Comment = comment;
        cred.UserName = user;
        cred.CredentialBlobSize = static_cast<DWORD>(secret.view().size() * sizeof(wchar_t));
        cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<wchar_t*>(secret.view().data()));
        cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
        if (!CredWriteW(&cred, 0))
            return win_fail(GetLastError());
        return {};
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::NoMemory);
    } catch (...) {
        return fail(ErrorCode::Internal);
    }
}

Result<SecretString> secret_get(std::wstring_view target) noexcept
{
    try {
        auto name = check_target(target);
        if (!name)
            return std::unexpected(std::move(name.error()));
        CREDENTIALW* raw = nullptr;
        if (!CredReadW(name->c_str(), CRED_TYPE_GENERIC, 0, &raw))
            return win_fail(GetLastError());
        const CredPtr cred(raw);
        const DWORD bytes = cred->CredentialBlobSize;
        if (bytes % sizeof(wchar_t) != 0 || bytes / sizeof(wchar_t) > kMaxSecretChars)
            return fail(ErrorCode::Format, L"bad credential blob");
        return SecretString::copy_of({reinterpret_cast<const wchar_t*>(cred->CredentialBlob), bytes / sizeof(wchar_t)});
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::NoMemory);
    } catch (...) {
        return fail(ErrorCode::Internal);
    }
}

Result<void> secret_delete(std::wstring_view target) noexcept
{
    try {
        auto name = check_target(target);
        if (!name)
            return std::unexpected(std::move(name.error()));
        if (!CredDeleteW(name->c_str(), CRED_TYPE_GENERIC, 0))
            return win_fail(GetLastError());
        return {};
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::NoMemory);
    } catch (...) {
        return fail(ErrorCode::Internal);
    }
}

}
