#include "Metadata.h"

#include "base/Handle.h"
#include "base/Text.h"
#include "doc/TxtToFb2.h"
#include "tts/LangDetect.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <optional>
#include <string>
#include <vector>

namespace eb {
namespace {

constexpr size_t kMetaCap = 1024;
constexpr size_t kSampleBytes = size_t{64} * 1024;
constexpr int32_t kSamplePages = 8;
constexpr float kLayoutW = 400, kLayoutH = 600, kLayoutEm = 12;

Error status_error(eb_status s)
{
    const auto text = utf8_to_wide(eb_status_text(s));
    return Error{error_from_status(s), text.value_or(std::wstring())};
}

std::wstring lower_ext(const std::filesystem::path& file)
{
    std::wstring e = file.extension().wstring();
    for (wchar_t& c : e)
        c = static_cast<wchar_t>(std::towlower(c));
    return e;
}

Result<std::vector<std::byte>> read_limited(const std::filesystem::path& file)
{
    const UniqueHandle h(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    LARGE_INTEGER size{};
    if (!h || GetFileType(h.get()) != FILE_TYPE_DISK || !GetFileSizeEx(h.get(), &size))
        return fail(ErrorCode::Io);
    if (size.QuadPart < 0 || static_cast<unsigned long long>(size.QuadPart) > kMaxTxtBytes)
        return fail(ErrorCode::Limit, L"text file too large");
    std::vector<std::byte> data(static_cast<size_t>(size.QuadPart));
    size_t done = 0;
    while (done < data.size()) {
        DWORD got = 0;
        if (!ReadFile(h.get(), data.data() + done, static_cast<DWORD>(std::min<size_t>(data.size() - done, 1U << 20)), &got, nullptr)
            || got == 0)
            return fail(ErrorCode::Io);
        done += got;
    }
    return data;
}

Result<UniqueDoc> open_text(const std::filesystem::path& file)
{
    auto raw = read_limited(file);
    if (!raw)
        return std::unexpected(std::move(raw.error()));
    auto fb2 = txt_to_fb2(*raw, file.stem().wstring());
    if (!fb2)
        return std::unexpected(std::move(fb2.error()));
    eb_doc* d = nullptr;
    const eb_status st = eb_open_memory(fb2->data(), fb2->size(), ".fb2", &d);
    UniqueDoc doc(d);
    if (st != EB_OK)
        return std::unexpected(status_error(st));
    return doc;
}

Result<UniqueDoc> open_file(const std::filesystem::path& file)
{
    eb_doc* d = nullptr;
    const eb_status st = eb_open(file.c_str(), &d);
    UniqueDoc doc(d);
    if (st != EB_OK)
        return std::unexpected(status_error(st));
    return doc;
}

std::optional<std::string> meta_string(eb_doc* doc, const char* key)
{
    std::string buf(kMetaCap, '\0');
    size_t needed = 0;
    if (eb_meta(doc, key, buf.data(), buf.size(), &needed) != EB_OK || needed < 2)
        return std::nullopt;
    buf.resize(std::min(needed, buf.size()) - 1);
    return buf;
}

std::string text_sample(eb_doc* doc)
{
    int32_t pages = 0;
    if (eb_page_count(doc, &pages) != EB_OK)
        return {};
    std::string s;
    for (int32_t p = 0; p < std::min(pages, kSamplePages) && s.size() < kSampleBytes; ++p) {
        eb_text* t = nullptr;
        if (eb_page_text(doc, p, &t) != EB_OK || t == nullptr)
            continue;
        s.append(t->utf8, t->utf8_len);
        eb_text_free(t);
    }
    size_t n = std::min(s.size(), kSampleBytes);
    while (n > 0 && n < s.size() && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80)
        --n;
    s.resize(n);
    return s;
}

std::wstring resolve_language(eb_doc* doc)
{
    const std::string tag = meta_string(doc, "language").value_or(std::string());
    const std::string sample = tag.empty() ? text_sample(doc) : std::string();
    if (tag.empty() && sample.empty())
        return {};
    return book_language(tag, sample) == Lang::Vi ? L"vi" : L"en";
}

BookMeta collect(eb_doc* doc, const std::filesystem::path& file, bool is_txt)
{
    BookMeta m;
    const auto wide = [](const std::optional<std::string>& s) {
        return s ? utf8_to_wide(*s).value_or(std::wstring()) : std::wstring();
    };
    m.title = wide(meta_string(doc, "title"));
    if (m.title.empty())
        m.title = file.stem().wstring();
    m.author = wide(meta_string(doc, "author"));
    m.lang = resolve_language(doc);
    m.fmt = is_txt ? L"txt" : lower_case(wide(meta_string(doc, "format")));
    if (m.fmt.empty()) {
        m.fmt = lower_ext(file);
        if (!m.fmt.empty())
            m.fmt.erase(0, 1);
    }
    return m;
}

} // namespace

ErrorCode error_from_status(eb_status s) noexcept
{
    switch (s) {
    case EB_OK:
    case EB_E_INTERNAL:
    case EB_E_ARG:
    case EB_E_RANGE:
    case EB_E_ABORTED:
        return ErrorCode::Internal;
    case EB_E_OPEN:
        return ErrorCode::Io;
    case EB_E_DRM:
        return ErrorCode::Access;
    case EB_E_LIMIT:
        return ErrorCode::Limit;
    case EB_E_NOMEM:
        return ErrorCode::NoMemory;
    default:
        return ErrorCode::Format;
    }
}

Result<UniqueDoc> open_book(const std::filesystem::path& file) noexcept
{
    try {
        auto doc = lower_ext(file) == L".txt" ? open_text(file) : open_file(file);
        if (!doc)
            return doc;
        if (eb_is_reflowable(doc->get()) != 0) {
            if (const eb_status ls = eb_layout(doc->get(), kLayoutW, kLayoutH, kLayoutEm, nullptr); ls != EB_OK)
                return std::unexpected(status_error(ls));
        }
        return doc;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::NoMemory);
    } catch (...) {
        return fail(ErrorCode::Internal);
    }
}

Result<BookMeta> read_book_meta(const std::filesystem::path& file) noexcept
{
    try {
        auto doc = open_book(file);
        if (!doc)
            return std::unexpected(std::move(doc.error()));
        return collect(doc->get(), file, lower_ext(file) == L".txt");
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::NoMemory);
    } catch (...) {
        return fail(ErrorCode::Internal);
    }
}

}
