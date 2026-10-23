#include <doctest/doctest.h>

#include "tts/Segmenter.h"

#include <ostream>
#include <string>
#include <vector>

namespace {

using V = std::vector<std::string>;

V split(std::string_view t)
{
    V r;
    for (const eb::Sentence& s : eb::split_sentences(t))
        r.push_back(eb::sentence_text(t, s));
    return r;
}

}

TEST_CASE("tts segmenter basic english")
{
    CHECK(split("Hello world. How are you? Fine!") == V{"Hello world.", "How are you?", "Fine!"});
    CHECK(split("").empty());
    CHECK(split("   \n  ").empty());
    CHECK(split("No terminator here") == V{"No terminator here"});
}

TEST_CASE("tts segmenter basic vietnamese")
{
    CHECK(split("Xin chào các bạn. Hôm nay trời đẹp quá! Bạn khỏe không?")
        == V{"Xin chào các bạn.", "Hôm nay trời đẹp quá!", "Bạn khỏe không?"});
}

TEST_CASE("tts segmenter offsets are byte ranges of the input")
{
    const std::string t = "  Một. Hai.";
    const auto s = eb::split_sentences(t);
    REQUIRE(s.size() == 2);
    CHECK(t.substr(s[0].begin, s[0].end - s[0].begin) == "Một.");
    CHECK(t.substr(s[1].begin, s[1].end - s[1].begin) == "Hai.");
}

TEST_CASE("tts segmenter english titles and abbreviations")
{
    CHECK(split("Mr. Smith met Dr. Jones. They talked.") == V{"Mr. Smith met Dr. Jones.", "They talked."});
    CHECK(split("Mrs. Brown and Ms. Green left St. Louis.") == V{"Mrs. Brown and Ms. Green left St. Louis."});
    CHECK(split("John Smith Jr. arrived. He sat.") == V{"John Smith Jr. arrived.", "He sat."});
    CHECK(split("Cats vs. dogs is old. Next topic.") == V{"Cats vs. dogs is old.", "Next topic."});
}

TEST_CASE("tts segmenter etc and e.g. i.e. U.S.")
{
    CHECK(split("We sold apples, pears, etc. and more. Done.") == V{"We sold apples, pears, etc. and more.", "Done."});
    CHECK(split("Use fruit, e.g. apples, daily. Ok.") == V{"Use fruit, e.g. apples, daily.", "Ok."});
    CHECK(split("That is, i.e. the end. Next.") == V{"That is, i.e. the end.", "Next."});
    CHECK(split("The U.S. economy grew. Prices fell.") == V{"The U.S. economy grew.", "Prices fell."});
}

TEST_CASE("tts segmenter vietnamese titles")
{
    CHECK(split("TS. Nguyễn Văn An và ThS. Lê Hoa đến. Họ nói chuyện.")
        == V{"TS. Nguyễn Văn An và ThS. Lê Hoa đến.", "Họ nói chuyện."});
    CHECK(split("GS. Trần và PGS. Lê gặp BS. Phạm. Xong rồi.") == V{"GS. Trần và PGS. Lê gặp BS. Phạm.", "Xong rồi."});
    CHECK(split("Tôi sống ở TP. Hồ Chí Minh. Rất đông.") == V{"Tôi sống ở TP. Hồ Chí Minh.", "Rất đông."});
    CHECK(split("Nhà ở Q. Bình Thạnh, P. 12. Gần chợ.") == V{"Nhà ở Q. Bình Thạnh, P. 12.", "Gần chợ."});
}

TEST_CASE("tts segmenter initials")
{
    CHECK(split("J. R. R. Tolkien wrote it. Good book.") == V{"J. R. R. Tolkien wrote it.", "Good book."});
    CHECK(split("I think so. I do.") == V{"I think so.", "I do."});
}

TEST_CASE("tts segmenter numbers")
{
    CHECK(split("Pi is 3.14 roughly. Next.") == V{"Pi is 3.14 roughly.", "Next."});
    CHECK(split("Dân số là 1.000.000 người. Rất lớn.") == V{"Dân số là 1.000.000 người.", "Rất lớn."});
    CHECK(split("It costs $5. Buy it.") == V{"It costs $5.", "Buy it."});
    CHECK(split("1. Mở đầu\nNội dung chính.") == V{"1. Mở đầu Nội dung chính."});
}

TEST_CASE("tts segmenter ellipsis")
{
    CHECK(split("Well... maybe not. Fine.") == V{"Well... maybe not.", "Fine."});
    CHECK(split("He paused... Then he left.") == V{"He paused...", "Then he left."});
    CHECK(split("Chờ đã… rồi đi. Xong.") == V{"Chờ đã… rồi đi.", "Xong."});
}

TEST_CASE("tts segmenter urls and emails")
{
    CHECK(split("See www.example.com for info. Thanks.") == V{"See www.example.com for info.", "Thanks."});
    CHECK(split("Mail a.b@c.com now. Bye.") == V{"Mail a.b@c.com now.", "Bye."});
    CHECK(split("Go to http://x.org/a.html. Then stop.") == V{"Go to http://x.org/a.html.", "Then stop."});
}

TEST_CASE("tts segmenter closing quotes and brackets")
{
    CHECK(split("He said \"Go home.\" Then he left.") == V{"He said \"Go home.\"", "Then he left."});
    CHECK(split("\"Really?\" she asked. Yes.") == V{"\"Really?\" she asked.", "Yes."});
    CHECK(split("(See above.) Next one.") == V{"(See above.)", "Next one."});
    CHECK(split("Cô nói: “Đi đi!” Rồi cô đi.") == V{"Cô nói: “Đi đi!”", "Rồi cô đi."});
}

TEST_CASE("tts segmenter wide terminators")
{
    CHECK(split("你好。再见！") == V{"你好。", "再见！"});
}

TEST_CASE("tts segmenter layout line joining")
{
    CHECK(split("This sentence is broken\nacross lines. Next one.")
        == V{"This sentence is broken across lines.", "Next one."});
    CHECK(split("Câu này bị ngắt\ndòng giữa chừng. Câu sau.") == V{"Câu này bị ngắt dòng giữa chừng.", "Câu sau."});
    CHECK(split("First line.\nSecond line.") == V{"First line.", "Second line."});
}

TEST_CASE("tts segmenter hyphenated line breaks")
{
    CHECK(split("An exam-\nple of joining. Done.") == V{"An example of joining.", "Done."});
    CHECK(split("Một ví dụ, hoàn-\ntoàn bình thường.") == V{"Một ví dụ, hoàntoàn bình thường."});
    CHECK(split("A well-\nKnown name here.") == V{"A well- Known name here."});
}

TEST_CASE("tts segmenter paragraph break splits")
{
    CHECK(split("Heading without stop\n\nBody text here.") == V{"Heading without stop", "Body text here."});
    CHECK(split("A\n \nB") == V{"A", "B"});
}

TEST_CASE("tts segmenter long sentences are bounded")
{
    std::string t;
    while (t.size() < 1500)
        t += "word, another ";
    const auto s = eb::split_sentences(t);
    REQUIRE(s.size() > 3);
    for (const auto& x : s)
        CHECK(x.end - x.begin <= eb::kMaxSentenceChars);
    CHECK(t[s[0].end - 1] == ',');
}

TEST_CASE("tts segmenter long text without spaces hard cuts at code points")
{
    std::string t;
    for (int i = 0; i < 900; ++i)
        t += "ệ";
    const auto s = eb::split_sentences(t);
    REQUIRE(s.size() == 3);
    CHECK(s[0].end - s[0].begin == 400 * 3);
    CHECK(s[2].end == t.size());
}

TEST_CASE("tts segmenter long vietnamese splits at space")
{
    std::string t;
    while (t.size() < 2000)
        t += "người Việt Nam ";
    const auto s = eb::split_sentences(t);
    REQUIRE(s.size() > 2);
    for (const auto& x : s) {
        const std::string p = eb::sentence_text(t, x);
        CHECK(p.size() <= 1200);
        CHECK((p.back() == 'm' || p.back() == 't' || p.back() == 'i'));
    }
}

TEST_CASE("tts segmenter invalid utf8 does not crash")
{
    const std::string t("ab\xC3\x28 cd. \xFF\xFE ef.");
    CHECK(eb::split_sentences(t).size() == 2);
}

TEST_CASE("tts furniture page numbers")
{
    CHECK(eb::strip_page_furniture("12\nBody text here.\n") == "Body text here.");
    CHECK(eb::strip_page_furniture("Body text here.\n\n  34  \n") == "Body text here.");
    CHECK(eb::strip_page_furniture("iv\nBody.\nxii") == "Body.");
    CHECK(eb::strip_page_furniture("- 7 -\nBody.") == "Body.");
    CHECK(eb::strip_page_furniture("Body.\nPage 9") == "Body.");
    CHECK(eb::strip_page_furniture("Body.\nTrang 10") == "Body.");
}

TEST_CASE("tts furniture keeps real content")
{
    CHECK(eb::strip_page_furniture("Body text.\nIn 1999 it ended.") == "Body text.\nIn 1999 it ended.");
    CHECK(eb::strip_page_furniture("CIVIL\nBody.") == "CIVIL\nBody.");
    CHECK(eb::strip_page_furniture("100 Days\nBody.") == "100 Days\nBody.");
    CHECK(eb::strip_page_furniture("Middle\n5\nlines here.") == "Middle\n5\nlines here.");
    CHECK(eb::strip_page_furniture("").empty());
    CHECK(eb::strip_page_furniture("42").empty());
}

TEST_CASE("tts furniture view points into input")
{
    const std::string t = "3\nHello.";
    const auto v = eb::strip_page_furniture(t);
    CHECK(v.data() == t.data() + 2);
}
