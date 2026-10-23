#include "eb_test_util.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {
bool any_page_has_text(eb_doc* d, int32_t count)
{
    for (int32_t p = 0; p < std::min<int32_t>(count, 20); ++p) {
        eb_text* raw = nullptr;
        REQUIRE(eb_page_text(d, p, &raw) == EB_OK);
        const TextPtr t(raw);
        if (t->utf8_len > 0) {
            CHECK(t->char_count > 0);
            CHECK(std::strlen(t->utf8) == t->utf8_len);
            CHECK(t->utf8_offset[0] == 0);
            return true;
        }
    }
    return false;
}

void check_bookmarks(eb_doc* d, int32_t count)
{
    const int32_t k = count / 2;
    eb_bookmark bm = 0;
    REQUIRE(eb_bookmark_of_page(d, k, &bm) == EB_OK);
    REQUIRE(eb_layout(d, 400, 600, 18, nullptr) == EB_OK);
    int32_t after = -1;
    int32_t n2 = 0;
    REQUIRE(eb_page_count(d, &n2) == EB_OK);
    REQUIRE(eb_page_of_bookmark(d, bm, &after) == EB_OK);
    CHECK(after >= 0);
    CHECK(after < n2);
    eb_bookmark bm2 = 0;
    int32_t again = -1;
    REQUIRE(eb_bookmark_of_page(d, after, &bm2) == EB_OK);
    REQUIRE(eb_page_of_bookmark(d, bm2, &again) == EB_OK);
    CHECK(again == after);
}

} // namespace

TEST_CASE("eb_mupdf reports ABI and engine versions")
{
    CHECK(eb_abi_version() == EB_ABI_VERSION);
    CHECK(std::strlen(eb_engine_version()) > 0);
}

TEST_CASE("eb_mupdf opens, lays out, renders and reads every good corpus file")
{
    const Lib lib;
    const auto files = corpus("good");
    REQUIRE_FALSE(files.empty());
    for (const auto& p : files) {
        INFO(p.string());
        const DocPtr d = open_doc(p);
        REQUIRE(eb_layout(d.get(), 400, 600, 12, nullptr) == EB_OK);
        int32_t count = 0;
        REQUIRE(eb_page_count(d.get(), &count) == EB_OK);
        REQUIRE(count > 0);

        const eb_render_opts plain{1.0f, 0, 0xFFFFFF, 0};
        const BitmapPtr bmp = render0(d.get(), plain);
        CHECK(bmp->width > 0);
        CHECK(bmp->stride == bmp->width * 4);
        CHECK(not_uniform(*bmp));
        CHECK(all_opaque(*bmp));

        if (!has_image_ext(p)) CHECK(any_page_has_text(d.get(), count));

        char buf[64] = {};
        size_t needed = 0;
        REQUIRE(eb_meta(d.get(), "format", buf, sizeof buf, &needed) == EB_OK);
        CHECK(buf[0] != '\0');
        CHECK(needed == std::strlen(buf) + 1);

        eb_toc_item* items = nullptr;
        int32_t n = -1;
        REQUIRE(eb_toc(d.get(), &items, &n) == EB_OK);
        CHECK(n >= 0);
        for (int32_t i = 0; i < n; ++i) CHECK(items[i].title != nullptr);
        eb_toc_free(items, n);

        float w = 0;
        float h = 0;
        REQUIRE(eb_page_size(d.get(), 0, &w, &h) == EB_OK);
        CHECK(w > 0);
        CHECK(h > 0);
        check_bookmarks(d.get(), count);
    }
}

namespace {

void poke_everything(eb_doc* d)
{
    eb_bitmap* c = nullptr;
    (void)eb_render_cover(d, 100, &c);
    const BitmapPtr coverGuard(c);
    eb_text* t = nullptr;
    (void)eb_page_text(d, 0, &t);
    const TextPtr textGuard(t);
    eb_toc_item* items = nullptr;
    int32_t n = 0;
    if (eb_toc(d, &items, &n) == EB_OK) eb_toc_free(items, n);
    char buf[64] = {};
    size_t needed = 0;
    for (const char* key : {"title", "author", "language", "format"}) (void)eb_meta(d, key, buf, sizeof buf, &needed);
    int32_t count = 0;
    if (eb_page_count(d, &count) == EB_OK && count > 0) {
        eb_bookmark bm = 0;
        int32_t page = 0;
        if (eb_bookmark_of_page(d, 0, &bm) == EB_OK) (void)eb_page_of_bookmark(d, bm, &page);
    }
    (void)eb_layout(d, 300, 500, 16, "p { color: red }");
}

} // namespace

TEST_CASE("eb_mupdf rejects bad corpus files without crashing")
{
    const Lib lib;
    const auto files = corpus("bad");
    REQUIRE_FALSE(files.empty());
    for (const auto& p : files) {
        INFO(p.string());
        eb_doc* raw = nullptr;
        const eb_status st = eb_open(p.c_str(), &raw);
        const DocPtr d(raw);
        if (st != EB_OK) {
            CHECK(raw == nullptr);
            continue;
        }
        (void)eb_layout(d.get(), 400, 600, 12, nullptr);
        eb_bitmap* b = nullptr;
        const eb_render_opts o{1.0f, 0, 0xFFFFFF, 0};
        const eb_status rs = eb_render_page(d.get(), 0, &o, &b);
        const BitmapPtr guard(b);
        const std::string name = p.filename().string();
        const bool tolerated = name == "badxml.fb2" || name == "truncated.mobi" || name == "zipbomb-ish.cbz";
        if (!tolerated) CHECK(rs != EB_OK);
        poke_everything(d.get());
    }
}

TEST_CASE("eb_mupdf enforces limits and argument checks")
{
    const auto files = corpus("good");
    REQUIRE_FALSE(files.empty());
    {
        eb_limits tiny{};
        tiny.max_file_bytes = 10;
        const Lib lib(&tiny);
        eb_doc* d = nullptr;
        CHECK(eb_open(files[0].c_str(), &d) == EB_E_LIMIT);
        CHECK(d == nullptr);
    }
    const Lib lib;
    eb_doc* none = nullptr;
    CHECK(eb_open(L"does-not-exist.epub", &none) == EB_E_OPEN);
    CHECK(eb_open(nullptr, &none) == EB_E_ARG);

    const DocPtr d = open_doc(files[0]);
    const eb_render_opts huge{1000.0f, 0, 0xFFFFFF, 0};
    eb_bitmap* b = nullptr;
    CHECK(eb_render_page(d.get(), 0, &huge, &b) == EB_E_LIMIT);
    CHECK(b == nullptr);
    const eb_render_opts ok{1.0f, 0, 0xFFFFFF, 0};
    CHECK(eb_render_page(d.get(), 100000, &ok, &b) == EB_E_RANGE);
    const DocPtr cbz = open_doc(first_with_ext(".cbz"));
    char buf[8] = {};
    size_t needed = 0;
    CHECK(eb_meta(cbz.get(), "language", buf, sizeof buf, &needed) == EB_E_RANGE);
    int32_t unused = 0;
    CHECK(eb_page_of_bookmark(d.get(), 0x7FFFFFF00000, &unused) != EB_OK);
}

TEST_CASE("eb_mupdf tint changes the rendered pixels")
{
    const Lib lib;
    const auto files = corpus("good");
    REQUIRE_FALSE(files.empty());
    const DocPtr d = open_doc(files[0]);
    const BitmapPtr plain = render0(d.get(), eb_render_opts{1.0f, 0, 0xFFFFFF, 0});
    const BitmapPtr tinted = render0(d.get(), eb_render_opts{1.0f, 0xE0E0E0, 0x101010, 1});
    REQUIRE(plain->width == tinted->width);
    REQUIRE(plain->height == tinted->height);
    const size_t bytes = static_cast<size_t>(plain->stride) * static_cast<size_t>(plain->height);
    CHECK(std::memcmp(plain->bgra, tinted->bgra, bytes) != 0);
    CHECK(all_opaque(*tinted));
}

TEST_CASE("eb_mupdf cover fits the requested side and memory open works")
{
    const Lib lib;
    const auto files = corpus("good");
    REQUIRE_FALSE(files.empty());
    const DocPtr d = open_doc(files[0]);
    eb_bitmap* raw = nullptr;
    REQUIRE(eb_render_cover(d.get(), 200, &raw) == EB_OK);
    const BitmapPtr cover(raw);
    CHECK(std::max(cover->width, cover->height) == 200);

    std::ifstream in(files[0], std::ios::binary);
    const std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE_FALSE(data.empty());
    eb_doc* m = nullptr;
    const std::string magic = files[0].extension().string();
    REQUIRE(eb_open_memory(data.data(), data.size(), magic.c_str(), &m) == EB_OK);
    const DocPtr mem(m);
    REQUIRE(eb_layout(mem.get(), 400, 600, 12, nullptr) == EB_OK);
    int32_t count = 0;
    CHECK(eb_page_count(mem.get(), &count) == EB_OK);
    CHECK(count > 0);

    const char junk[] = "this is not a document";
    eb_doc* bad = nullptr;
    CHECK(eb_open_memory(junk, sizeof junk, ".epub", &bad) != EB_OK);
    CHECK(bad == nullptr);
}

TEST_CASE("eb_mupdf rejects device paths and non-files")
{
    const Lib lib;
    const auto files = corpus("good");
    REQUIRE_FALSE(files.empty());
    eb_doc* d = nullptr;
    for (const wchar_t* bad : {L"NUL", L"nul.epub", L"C:\\temp\\con.txt", L"COM1", L"lpt9.txt", L"\\\\.\\C:",
                               L"\\\\.\\pipe\\x", L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolume1\\x", L"aux ."}) {
        INFO(fs::path(bad).string());
        CHECK(eb_open(bad, &d) == EB_E_ARG);
        CHECK(d == nullptr);
    }
    const fs::path dir = files[0].parent_path();
    CHECK(eb_open(dir.c_str(), &d) == EB_E_OPEN);
    const std::wstring extended = L"\\\\?\\" + fs::absolute(files[0]).make_preferred().wstring();
    REQUIRE(eb_open(extended.c_str(), &d) == EB_OK);
    eb_close(d);
}

namespace {

eb_status first_failure_of_memory_doc(const std::string& data, const char* magic)
{
    eb_doc* raw = nullptr;
    eb_status st = eb_open_memory(data.data(), data.size(), magic, &raw);
    const DocPtr d(raw);
    if (st != EB_OK) return st;
    st = eb_layout(d.get(), 400, 600, 12, nullptr);
    if (st != EB_OK) return st;
    eb_bitmap* b = nullptr;
    const eb_render_opts o{1.0f, 0, 0xFFFFFF, 0};
    st = eb_render_page(d.get(), 0, &o, &b);
    eb_bitmap_free(b);
    return st;
}

} // namespace

TEST_CASE("eb_mupdf heap cap yields EB_E_NOMEM without crashing")
{
    fs::path epub;
    for (const auto& f : corpus("good")) {
        if (f.extension() == ".epub") {
            epub = f;
            break;
        }
    }
    REQUIRE_FALSE(epub.empty());
    std::ifstream in(epub, std::ios::binary);
    const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    eb_limits lim{};
    lim.max_heap_bytes = 1500000;
    eb_status st = eb_init(&lim);
    if (st == EB_OK) {
        st = first_failure_of_memory_doc(data, ".epub");
        eb_shutdown();
    }
    INFO(eb_status_text(st) << ": " << eb_last_error());
    CHECK(st == EB_E_NOMEM);
}

namespace {

std::string heavy_pdf()
{
    std::string content = "0.4 w [3 2] 0 d\n";
    for (int i = 0; i < 400000; ++i) {
        content += std::to_string(i % 600) + " 0 m 612 " + std::to_string(i % 790) + " l S\n";
    }
    std::string pdf =
        "%PDF-1.4\n1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1>>endobj\n"
        "3 0 obj<</Type/Page/Parent 2 0 R/MediaBox[0 0 612 792]/Contents 4 0 R>>endobj\n4 0 obj<</Length " +
        std::to_string(content.size()) + ">>stream\n";
    pdf += content;
    pdf += "\nendstream endobj\ntrailer<</Root 1 0 R>>\n%%EOF\n";
    return pdf;
}

} // namespace

TEST_CASE("eb_mupdf eb_abort cancels a running render from another thread")
{
    const Lib lib;
    const std::string pdf = heavy_pdf();
    eb_doc* raw = nullptr;
    REQUIRE(eb_open_memory(pdf.data(), pdf.size(), ".pdf", &raw) == EB_OK);
    const DocPtr d(raw);
    std::atomic<int> status{-1};
    std::thread worker([&] {
        eb_bitmap* b = nullptr;
        const eb_render_opts o{3.0f, 0, 0xFFFFFF, 0};
        status = static_cast<int>(eb_render_page(d.get(), 0, &o, &b));
        eb_bitmap_free(b);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    eb_abort(d.get());
    worker.join();
    CHECK(status == EB_E_ABORTED);
    const eb_render_opts again{0.2f, 0, 0xFFFFFF, 0};
    eb_bitmap* b = nullptr;
    CHECK(eb_render_page(d.get(), 0, &again, &b) == EB_OK);
    eb_bitmap_free(b);
}

TEST_CASE("eb_mupdf documents on different threads render concurrently")
{
    const Lib lib;
    const auto files = corpus("good");
    REQUIRE_FALSE(files.empty());
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(4);
    for (size_t t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            eb_doc* raw = nullptr;
            if (eb_open(files[t % files.size()].c_str(), &raw) != EB_OK) {
                ++failures;
                return;
            }
            const DocPtr d(raw);
            int32_t count = 0;
            if (eb_layout(d.get(), 400, 600, 12, nullptr) != EB_OK || eb_page_count(d.get(), &count) != EB_OK) ++failures;
            const eb_render_opts o{1.0f, 0, 0xFFFFFF, 0};
            for (int32_t p = 0; p < std::min<int32_t>(count, 5); ++p) {
                eb_bitmap* b = nullptr;
                eb_text* tx = nullptr;
                if (eb_render_page(d.get(), p, &o, &b) != EB_OK) ++failures;
                if (eb_page_text(d.get(), p, &tx) != EB_OK) ++failures;
                eb_bitmap_free(b);
                eb_text_free(tx);
            }
        });
    }
    for (auto& th : threads) th.join();
    CHECK(failures == 0);
}
