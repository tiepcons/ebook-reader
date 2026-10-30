#include <doctest/doctest.h>

#include "../integration/eb_test_epub.h"
#include "test_base_tmp.h"
#include "ui/ReaderSpread.h"
#include "ui/ReaderWorker.h"

#include <chrono>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kClientW = 500;
constexpr int kClientH = 260;
constexpr size_t kChapters = 24;

struct Engine {
    Engine() { REQUIRE(eb_init(nullptr) == EB_OK); }
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    ~Engine() { eb_shutdown(); }
};

struct Notify {
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    Notify() { REQUIRE(hwnd != nullptr); }
    Notify(const Notify&) = delete;
    Notify& operator=(const Notify&) = delete;
    ~Notify() { DestroyWindow(hwnd); }
};

uint64_t chapter_mark(size_t chapter)
{
    return static_cast<uint64_t>(chapter) << 32;
}

// The numbering the book ends up with: an eager layout of the same size.
struct Truth {
    eb_doc* doc = nullptr;
    int32_t total = 0;
    explicit Truth(const std::string& epub)
    {
        REQUIRE(eb_open_memory(epub.data(), epub.size(), ".epub", &doc) == EB_OK);
        const eb::LayoutParams lp = eb::reflow_layout(kClientW, kClientH, 96);
        REQUIRE(eb_layout(doc, lp.width_pt, lp.height_pt, eb::ReaderStyle{}.em_pt, nullptr) == EB_OK);
        REQUIRE(eb_page_count(doc, &total) == EB_OK);
    }
    Truth(const Truth&) = delete;
    Truth& operator=(const Truth&) = delete;
    ~Truth() { eb_close(doc); }
    [[nodiscard]] int32_t page_of(uint64_t mark) const
    {
        int32_t p = -1;
        REQUIRE(eb_page_of_bookmark(doc, mark, &p) == EB_OK);
        return p;
    }
    [[nodiscard]] std::string text_of(int32_t page) const
    {
        eb_text* t = nullptr;
        REQUIRE(eb_page_text(doc, page, &t) == EB_OK);
        std::string s(t->utf8, t->utf8_len);
        eb_text_free(t);
        return s;
    }
};

// What the reader view does with the results: follows page numbers through Layout results and repeats requests.
struct FakeView {
    explicit FakeView(eb::RenderWorker& w) : worker(w) {}
    eb::RenderWorker& worker;
    eb::RenderWant want;
    int32_t page = -1, count = 0, first_count = 0;
    uint64_t mark = 0;
    uint32_t numbering = 0;
    bool complete = false, toc_changed = false;
    int renumberings = 0;
    int32_t shift_sum = 0;
    std::vector<int32_t> pending_texts;
    std::string text_seen;

    void send()
    {
        want.numbering = numbering;
        want.page = page < 0 ? 0 : page;
        want.mark = page < 0 ? eb::kNoAnchor : mark;
        worker.request(want);
    }

    void on_layout(const eb::RenderResult& r)
    {
        if (r.numbering != numbering) {
            numbering = r.numbering;
            ++renumberings;
            shift_sum += r.shift;
            page = eb::renumbered_page(page, r.shift, count, r.page_count, 1);
            for (int32_t& p : pending_texts)
                p = std::max(p + r.shift, 0);
            for (const int32_t p : pending_texts)
                worker.request_text(p, numbering);
            count = r.page_count;
            send();
        }
        count = r.page_count;
        complete = r.complete;
        toc_changed = toc_changed || r.toc_changed;
    }

    void handle(eb::RenderResult& r)
    {
        if (r.kind == eb::RenderResult::Kind::Layout) {
            on_layout(r);
        } else if (r.kind == eb::RenderResult::Kind::Text) {
            std::erase(pending_texts, r.page);
            if (r.text)
                text_seen = r.text->utf8;
        } else if (r.kind == eb::RenderResult::Kind::Page && r.primary && r.status == EB_OK) {
            if (first_count == 0)
                first_count = r.page_count;
            page = r.page;
            mark = r.mark;
            count = r.page_count;
            if (pending_texts.empty() && text_seen.empty()) {
                pending_texts.push_back(page + 1);
                worker.request_text(page + 1, numbering);
            }
        }
    }

    bool pump(std::chrono::seconds limit)
    {
        const auto end = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < end) {
            for (eb::RenderResult& r : worker.take())
                handle(r);
            if (complete && pending_texts.empty())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
};

}

TEST_CASE("reader worker: the book is laid out in the background, the place and the read-aloud text stay right")
{
    const Engine engine;
    const Notify notify;
    const ebtest::TempDir dir;
    const std::string epub = ebt::multi_chapter_epub({kChapters, false, true});
    const std::filesystem::path book = dir.path() / L"book.epub";
    {
        std::ofstream out(book, std::ios::binary);
        out.write(epub.data(), static_cast<std::streamsize>(epub.size()));
        REQUIRE(out.good());
    }
    const Truth truth(epub);
    const size_t start_chapter = 12;
    eb::RenderWorker worker(notify.hwnd, WM_APP + 1, eb::OpenJob{book, dir.path() / L"pending", "abc", "epub", {}});
    FakeView view(worker);
    view.want.view = {kClientW, kClientH, 96, 1, {}};
    view.want.anchor = chapter_mark(start_chapter);
    view.send();
    REQUIRE(view.pump(std::chrono::seconds(60)));
    CHECK(view.count == truth.total);
    CHECK(view.first_count != truth.total);
    CHECK(view.renumberings > 0);
    CHECK(view.mark == chapter_mark(start_chapter));
    CHECK(view.page == truth.page_of(view.mark));
    CHECK(view.text_seen == truth.text_of(view.page + 1));
    CHECK(view.toc_changed);
    const std::vector<eb::TocItem> toc = worker.toc();
    REQUIRE(toc.size() == kChapters / 3);
    for (size_t i = 0; i < toc.size(); ++i)
        CHECK(toc[i].target == chapter_mark(i * 3));
}

TEST_CASE("reader worker: a request made in an older numbering is dropped, not answered with another page")
{
    const Engine engine;
    const Notify notify;
    const ebtest::TempDir dir;
    const std::string epub = ebt::multi_chapter_epub({kChapters});
    const std::filesystem::path book = dir.path() / L"book.epub";
    {
        std::ofstream out(book, std::ios::binary);
        out.write(epub.data(), static_cast<std::streamsize>(epub.size()));
        REQUIRE(out.good());
    }
    eb::RenderWorker worker(notify.hwnd, WM_APP + 1, eb::OpenJob{book, dir.path() / L"pending", "abc", "epub", {}});
    FakeView view(worker);
    view.want.view = {kClientW, kClientH, 96, 1, {}};
    view.want.anchor = chapter_mark(12);
    view.send();
    REQUIRE(view.pump(std::chrono::seconds(60)));
    REQUIRE(view.numbering > 0);
    worker.request_text(1, view.numbering - 1);
    worker.request_text(2, view.numbering);
    std::vector<int32_t> answered;
    for (int i = 0; i < 400 && answered.empty(); ++i) {
        for (const eb::RenderResult& r : worker.take()) {
            if (r.kind == eb::RenderResult::Kind::Text)
                answered.push_back(r.page);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(answered.size() == 1);
    CHECK(answered[0] == 2);
}
