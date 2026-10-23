#include <doctest/doctest.h>

#include "base/CrashGuard.h"
#include "base/Hash.h"
#include "lib/Scanner.h"
#include "lib/Schema.h"
#include "test_base_tmp.h"

#include <chrono>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct Fixture {
    ebtest::TempDir root;
    ebtest::TempDir data;
    eb::Db db = open();
    eb::CrashLoopGuard guard{data.path() / L"marker.txt"};
    std::atomic<bool> cancel{false};
    int calls = 0;
    eb::MetaFn meta = [this](const fs::path& p) -> eb::Result<eb::BookMeta> {
        ++calls;
        return eb::BookMeta{L"T " + p.stem().wstring(), L"Tác Giả", L"vi", L"epub"};
    };

    eb::Db open()
    {
        auto d = eb::Db::open(data.path() / L"lib.db");
        REQUIRE(d.has_value());
        REQUIRE(eb::migrate(*d).has_value());
        return std::move(*d);
    }

    eb::ScanStats scan(std::span<const std::wstring> ex = {})
    {
        const fs::path roots[] = {root.path()};
        auto r = eb::scan_library(db, roots, meta, guard, cancel, {}, ex);
        REQUIRE(r.has_value());
        return *r;
    }

    eb::ScanStats scan_roots(std::vector<fs::path> roots)
    {
        auto r = eb::scan_library(db, roots, meta, guard, cancel);
        REQUIRE(r.has_value());
        return *r;
    }

    int64_t count(const std::string& where)
    {
        auto st = db.prepare("SELECT COUNT(*) FROM books WHERE " + where);
        REQUIRE(st.has_value());
        REQUIRE(st->step().has_value());
        return st->column_int(0);
    }

    std::string text(const std::string& sql)
    {
        auto st = db.prepare(sql);
        REQUIRE(st.has_value());
        REQUIRE(st->step().value() == eb::Step::Row);
        return std::string(st->column_text(0));
    }
};

fs::path make(const fs::path& p, const std::string& content)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << content;
    return p;
}

void bump_mtime(const fs::path& p)
{
    fs::last_write_time(p, fs::last_write_time(p) + std::chrono::seconds(30));
}

bool make_junction(const fs::path& link, const fs::path& target)
{
    std::wstring cmd = L"cmd.exe /c mklink /J \"" + link.wstring() + L"\" \"" + target.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, 10000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return fs::exists(link);
}

}

TEST_CASE("scanner: first scan adds, rescan is free")
{
    Fixture f;
    make(f.root.path() / L"a.epub", "aaa");
    make(f.root.path() / L"sub" / L"b.PDF", "bbb");
    make(f.root.path() / L"sub" / L"c.txt", "ccc");
    make(f.root.path() / L"sub" / L"skip.docx", "ddd");
    auto s = f.scan();
    CHECK(s.added == 3);
    CHECK(f.calls == 3);
    CHECK(f.count("status=0") == 3);
    CHECK(f.text("SELECT title_nd FROM books WHERE title='T a'") == "t a");
    CHECK(f.text("SELECT fmt FROM books WHERE title='T c'") == "epub");

    f.calls = 0;
    s = f.scan();
    CHECK(f.calls == 0);
    CHECK(s.unchanged == 3);
    CHECK(s.added == 0);
}

TEST_CASE("scanner: touched file is updated without new meta")
{
    Fixture f;
    const auto p = make(f.root.path() / L"a.epub", "aaa");
    f.scan();
    bump_mtime(p);
    f.calls = 0;
    const auto s = f.scan();
    CHECK(s.updated == 1);
    CHECK(f.calls == 0);
    CHECK(f.count("1=1") == 1);
}

TEST_CASE("scanner: move keeps the id and progress")
{
    Fixture f;
    const auto p = make(f.root.path() / L"a.epub", "aaa");
    f.scan();
    const std::string id = f.text("SELECT id FROM books");
    REQUIRE(f.db.exec(("INSERT INTO progress(book_id,bookmark,percent,updated_at) VALUES('" + id + "',5,0.5,1)").c_str()).has_value());
    fs::create_directories(f.root.path() / L"moved");
    fs::rename(p, f.root.path() / L"moved" / L"z.epub");
    f.calls = 0;
    const auto s = f.scan();
    CHECK(s.moved == 1);
    CHECK(s.missing == 0);
    CHECK(f.calls == 0);
    CHECK(f.count("1=1") == 1);
    CHECK(f.text("SELECT id FROM books") == id);
    CHECK(f.text("SELECT path FROM books").ends_with("z.epub"));
    CHECK(f.text("SELECT COUNT(*) FROM progress") == "1");
}

TEST_CASE("scanner: delete marks missing, restore clears it")
{
    Fixture f;
    const auto p = make(f.root.path() / L"a.epub", "aaa");
    make(f.root.path() / L"b.epub", "bbb");
    f.scan();
    const fs::path hold = f.data.path() / L"a.epub";
    fs::rename(p, hold);
    CHECK(f.scan().missing == 1);
    CHECK(f.count("status=1") == 1);
    CHECK(f.scan().missing == 0);
    fs::rename(hold, p);
    f.calls = 0;
    f.scan();
    CHECK(f.count("status=0") == 2);
    CHECK(f.calls == 0);
}

TEST_CASE("scanner: duplicate copy gets no second row")
{
    Fixture f;
    make(f.root.path() / L"a.epub", "same");
    make(f.root.path() / L"copy" / L"a2.epub", "same");
    const auto s = f.scan();
    CHECK(s.added == 1);
    CHECK(s.unchanged == 1);
    CHECK(f.count("1=1") == 1);
    CHECK(f.scan().missing == 0);
}

TEST_CASE("scanner: junction is not followed")
{
    Fixture f;
    const ebtest::TempDir outside;
    make(outside.path() / L"x.epub", "outside");
    make(f.root.path() / L"in.epub", "inside");
    if (!make_junction(f.root.path() / L"jn", outside.path())) {
        MESSAGE("junction creation not permitted, test skipped");
        return;
    }
    const auto s = f.scan();
    CHECK(s.added == 1);
    CHECK(f.count("path LIKE '%x.epub'") == 0);
}

TEST_CASE("scanner: hidden files and excluded directories are skipped")
{
    Fixture f;
    make(f.root.path() / L"a.epub", "aaa");
    const auto h = make(f.root.path() / L"h.epub", "hhh");
    SetFileAttributesW(h.c_str(), FILE_ATTRIBUTE_HIDDEN);
    make(f.root.path() / L"Fonts" / L"f.epub", "fff");
    const std::wstring ex[] = {L"fonts"};
    CHECK(f.scan(ex).added == 1);
}

TEST_CASE("scanner: crash suspect is marked bad without calling meta")
{
    Fixture f;
    const auto p = make(f.root.path() / L"a.epub", "aaa");
    make(f.root.path() / L"b.epub", "bbb");
    const std::string id = eb::book_id(p).value();
    {
        eb::CrashLoopGuard crashed(f.data.path() / L"marker.txt");
        REQUIRE(crashed.begin("scan", id).has_value());
    }
    eb::CrashLoopGuard after(f.data.path() / L"marker.txt");
    REQUIRE(after.previous_suspect() == id);
    const fs::path roots[] = {f.root.path()};
    const auto s = eb::scan_library(f.db, roots, f.meta, after, f.cancel).value();
    CHECK(s.added == 1);
    CHECK(s.bad == 1);
    CHECK(f.calls == 1);
    CHECK(f.count("status=2") == 1);
}

TEST_CASE("scanner: meta failure marks the book bad with the file stem")
{
    Fixture f;
    make(f.root.path() / L"Sách hay.epub", "aaa");
    f.meta = [](const fs::path&) -> eb::Result<eb::BookMeta> { return eb::fail(eb::ErrorCode::Format); };
    const auto s = f.scan();
    CHECK(s.bad == 1);
    CHECK(f.count("status=2 AND fmt='epub'") == 1);
    CHECK(f.text("SELECT title_nd FROM books") == "sach hay");
}

TEST_CASE("scanner: Vietnamese names and folders")
{
    Fixture f;
    make(f.root.path() / L"Truyện Kiếm Hiệp" / L"Thiên Long Bát Bộ.epub", "tlbb");
    CHECK(f.scan().added == 1);
    CHECK(f.text("SELECT folder FROM books").ends_with("Truyện Kiếm Hiệp"));
    CHECK(f.text("SELECT title FROM books") == "T Thiên Long Bát Bộ");
    CHECK(f.scan().unchanged == 1);
}

TEST_CASE("scanner: cancel commits a consistent partial result")
{
    Fixture f;
    for (int i = 0; i < 10; ++i)
        make(f.root.path() / (L"b" + std::to_wstring(i) + L".epub"), "book" + std::to_string(i));
    f.meta = [&f](const fs::path& p) -> eb::Result<eb::BookMeta> {
        if (++f.calls == 3)
            f.cancel = true;
        return eb::BookMeta{p.stem().wstring(), {}, {}, {}};
    };
    const auto s = f.scan();
    CHECK(s.added == 3);
    CHECK(f.count("status=0") == 3);
    CHECK(f.count("status=1") == 0);
    f.cancel = false;
    CHECK(f.scan().added == 7);
    CHECK(f.count("1=1") == 10);
}

TEST_CASE("scanner: changed content gets a new id and keeps progress")
{
    Fixture f;
    const auto p = make(f.root.path() / L"a.txt", "short");
    f.scan();
    const std::string old = f.text("SELECT id FROM books");
    REQUIRE(f.db.exec(("INSERT INTO progress(book_id,bookmark,percent,updated_at) VALUES('" + old + "',5,0.5,1)").c_str()).has_value());
    make(p, "much longer content now");
    bump_mtime(p);
    const auto s = f.scan();
    CHECK(s.updated == 1);
    CHECK(f.count("1=1") == 1);
    CHECK(f.text("SELECT id FROM books") != old);
    CHECK(f.text("SELECT COUNT(*) FROM progress p JOIN books b ON b.id=p.book_id") == "1");
}

TEST_CASE("scanner: no write transaction is held while meta runs")
{
    Fixture f;
    for (int i = 0; i < 8; ++i)
        make(f.root.path() / (L"s" + std::to_wstring(i) + L".epub"), "slow" + std::to_string(i));
    bool wrote = false;
    f.meta = [&](const fs::path& p) -> eb::Result<eb::BookMeta> {
        Sleep(50);
        if (++f.calls == 5) {
            auto other = eb::Db::open(f.data.path() / L"lib.db");
            wrote = other.has_value() && other->exec("INSERT INTO kv(key,value) VALUES('k','v')").has_value();
        }
        return eb::BookMeta{p.stem().wstring(), {}, {}, {}};
    };
    CHECK(f.scan().added == 8);
    CHECK(wrote);
}

TEST_CASE("scanner: other roots and offline roots are left alone")
{
    Fixture f;
    ebtest::TempDir b;
    const auto pa = make(f.root.path() / L"a.epub", "aaa");
    make(b.path() / L"b.epub", "bbb");
    CHECK(f.scan_roots({f.root.path(), b.path()}).added == 2);
    CHECK(f.scan_roots({f.root.path()}).missing == 0);
    CHECK(f.count("status=0") == 2);
    fs::remove(pa);
    CHECK(f.scan_roots({f.root.path()}).missing == 1);
    const fs::path gone = f.data.path() / L"offline";
    fs::rename(b.path(), gone);
    CHECK(f.scan_roots({f.root.path(), b.path()}).missing == 0);
    CHECK(f.count("status=1") == 1);
    fs::rename(gone, b.path());
}

TEST_CASE("scanner: root casing does not cause rehash")
{
    Fixture f;
    make(f.root.path() / L"Sub" / L"a.epub", "aaa");
    f.scan();
    std::wstring up = f.root.path().wstring();
    for (auto& c : up)
        c = static_cast<wchar_t>(towupper(c));
    f.calls = 0;
    const auto s = f.scan_roots({fs::path(up)});
    CHECK(s.unchanged == 1);
    CHECK(s.updated == 0);
    CHECK(s.added == 0);
    CHECK(f.calls == 0);
    CHECK(f.count("1=1") == 1);
}

TEST_CASE("scanner: changed file equal to a vanished book relocates that book")
{
    Fixture f;
    const auto a = make(f.root.path() / L"a.txt", "content-one");
    const auto b = make(f.root.path() / L"b.txt", "content-two");
    f.scan();
    const std::string idb = f.text("SELECT id FROM books WHERE path LIKE '%b.txt'");
    fs::remove(b);
    make(a, "content-two");
    bump_mtime(a);
    const auto s = f.scan();
    CHECK(s.moved == 1);
    CHECK(f.count("1=1") == 2);
    CHECK(f.text("SELECT path FROM books WHERE id='" + idb + "'").ends_with("a.txt"));
    CHECK(f.count("status=0") == 1);
}

TEST_CASE("scanner: changed file equal to a live book becomes a duplicate and is not rehashed")
{
    Fixture f;
    const auto a = make(f.root.path() / L"a.txt", "content-one");
    make(f.root.path() / L"b.txt", "content-two");
    f.scan();
    make(a, "content-two");
    bump_mtime(a);
    auto s = f.scan();
    CHECK(s.bad == 1);
    CHECK(f.count("status=2") == 1);
    s = f.scan();
    CHECK(s.bad == 0);
    CHECK(s.unchanged == 2);
}

TEST_CASE("scanner: a hidden book stays hidden and is never re-read")
{
    Fixture f;
    const auto p = make(f.root.path() / L"a.epub", "aaa");
    f.scan();
    REQUIRE(f.db.exec("UPDATE books SET status=3").has_value());
    bump_mtime(p);
    f.calls = 0;
    f.scan();
    f.scan();
    CHECK(f.calls == 0);
    CHECK(f.count("status=3") == 1);
    CHECK(f.count("1=1") == 1);
}

TEST_CASE("scanner: throwing meta marks the book bad")
{
    Fixture f;
    make(f.root.path() / L"a.epub", "aaa");
    f.meta = [](const fs::path&) -> eb::Result<eb::BookMeta> { throw std::runtime_error("boom"); };
    CHECK(f.scan().bad == 1);
    CHECK(f.count("status=2") == 1);
}

TEST_CASE("scanner: path helpers keep drive roots intact")
{
    CHECK(eb::scan_path::pattern(L"D:\\") == L"D:\\*");
    CHECK(eb::scan_path::pattern(L"D:\\books") == L"D:\\books\\*");
    CHECK(eb::scan_path::join(L"D:\\", L"a.epub") == L"D:\\a.epub");
    CHECK(eb::scan_path::strip_long_prefix(L"\\\\?\\C:\\x") == L"C:\\x");
    CHECK(eb::scan_path::strip_long_prefix(L"\\\\?\\UNC\\h\\s\\x") == L"\\\\h\\s\\x");
}

#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__)
TEST_CASE("scanner: 1000 files, rescan under one second")
{
    Fixture f;
    for (int i = 0; i < 1000; ++i)
        make(f.root.path() / (L"d" + std::to_wstring(i % 20)) / (L"f" + std::to_wstring(i) + L".epub"), "x" + std::to_string(i));
    CHECK(f.scan().added == 1000);
    const auto t0 = std::chrono::steady_clock::now();
    const auto s = f.scan();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(s.unchanged == 1000);
    CHECK(ms < 1000);
    MESSAGE("rescan ms: " << ms);
}
#endif
