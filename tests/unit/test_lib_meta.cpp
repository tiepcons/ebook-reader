#include "../integration/eb_test_util.h"
#include "lib/Metadata.h"
#include "test_base_tmp.h"

#include <fstream>
#include <string>

namespace {

eb::BookMeta meta_of(const fs::path& p)
{
    auto r = eb::read_book_meta(p);
    INFO(p.string());
    REQUIRE(r.has_value());
    return *r;
}

}

TEST_CASE("metadata: every good corpus book reports a title and a format")
{
    const Lib lib;
    for (const auto& p : corpus("good")) {
        const auto ext = p.extension().string();
        if (ext == ".cbz")
            continue;
        const auto m = meta_of(p);
        INFO(p.string());
        CHECK_FALSE(m.title.empty());
        CHECK_FALSE(m.fmt.empty());
        if (ext != ".pdf")
            CHECK_FALSE(m.lang.empty());
    }
}

TEST_CASE("metadata: language comes from tags or text")
{
    const Lib lib;
    for (const auto& p : corpus("good")) {
        const std::string name = p.filename().string();
        if (name.starts_with("truyen-kieu")) {
            INFO(name);
            CHECK(meta_of(p).lang == L"vi");
        } else if (name == "pg11.epub" || name == "sample-en.fb2") {
            CHECK(meta_of(p).lang == L"en");
        } else if (name == "sample-vi.fb2") {
            CHECK(meta_of(p).lang == L"vi");
        }
    }
}

TEST_CASE("metadata: txt keeps its own format name")
{
    const Lib lib;
    const auto m = meta_of(first_with_ext(".txt"));
    CHECK(m.fmt == L"txt");
}

TEST_CASE("metadata: corrupt and missing files are errors")
{
    const Lib lib;
    const ebtest::TempDir dir;
    const fs::path junk = dir.path() / L"junk.epub";
    std::ofstream(junk, std::ios::binary) << "this is not a zip";
    CHECK_FALSE(eb::read_book_meta(junk).has_value());
    CHECK_FALSE(eb::read_book_meta(dir.path() / L"nope.pdf").has_value());
    CHECK_FALSE(eb::read_book_meta(dir.path() / L"nope.txt").has_value());
}
