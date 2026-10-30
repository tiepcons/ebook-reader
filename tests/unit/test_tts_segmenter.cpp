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
    CHECK(split("Well... maybe not. Fine.") == V{"Well, maybe not.", "Fine."});
    CHECK(split("He paused... Then he left.") == V{"He paused.", "Then he left."});
    CHECK(split("Chờ đã… rồi đi. Xong.") == V{"Chờ đã, rồi đi.", "Xong."});
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
    CHECK(split("An inter-\nnational trip.") == V{"An international trip."});
    CHECK(split("Một ví dụ, hoàn-\ntoàn bình thường.") == V{"Một ví dụ, hoàn-toàn bình thường."});
    CHECK(split("Ở Hà-\nNội nóng.") == V{"Ở Hà-Nội nóng."});
    CHECK(split("A well-\nKnown name here.") == V{"A well-Known name here."});
    CHECK(split("The re-\nsult is odd-\nly fine.") == V{"The result is oddly fine."});
    CHECK(split("A co\xC2\xADoperative plan.") == V{"A cooperative plan."});
}

TEST_CASE("tts segmenter text ending with terminator and whitespace")
{
    CHECK(split("Hello.\n") == V{"Hello."});
    CHECK(split("Hello. ") == V{"Hello."});
    CHECK(split("Xin chào!\r\n") == V{"Xin chào!"});
    CHECK(split("Hello.\n\n\n") == V{"Hello."});
}

TEST_CASE("tts segmenter compound vietnamese titles")
{
    CHECK(split("PGS.TS. Nguyễn Văn A đến. Xong.") == V{"PGS.TS. Nguyễn Văn A đến.", "Xong."});
    CHECK(split("GS.TS. Lê Hoa nói. Xong.") == V{"GS.TS. Lê Hoa nói.", "Xong."});
    CHECK(split("ThS.BS. Trần An khám. Xong.") == V{"ThS.BS. Trần An khám.", "Xong."});
}

TEST_CASE("tts segmenter cjk cut points and combining marks")
{
    std::string t;
    for (int i = 0; i < 300; ++i)
        t += "字，";
    for (const auto& x : eb::split_sentences(t))
        CHECK(x.end - x.begin <= 3 * eb::kMaxSentenceChars);
    std::string m;
    for (int i = 0; i < 399; ++i)
        m += "a";
    m += "e\xCC\x82 tail";
    const auto s = eb::split_sentences(m);
    REQUIRE(s.size() >= 1);
    CHECK(m.compare(s[0].end - 2, 2, "\xCC\x82") == 0);
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
    CHECK(eb::strip_page_furniture("IV\nBody.\nXII") == "Body.");
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
    CHECK(eb::strip_page_furniture("mix\nBody.") == "mix\nBody.");
    CHECK(eb::strip_page_furniture("dim\nBody.") == "dim\nBody.");
    CHECK(eb::strip_page_furniture("vi\nBody.") == "vi\nBody.");
    CHECK(eb::strip_page_furniture("Page iv\nBody.") == "Body.");
    CHECK(eb::strip_page_furniture("").empty());
    CHECK(eb::strip_page_furniture("42").empty());
}

TEST_CASE("tts furniture view points into input")
{
    const std::string t = "3\nHello.";
    const auto v = eb::strip_page_furniture(t);
    CHECK(v.data() == t.data() + 2);
}

TEST_CASE("tts segmenter ellipses are pauses, never spoken as dots")
{
    CHECK(split("Wait... what is it?") == V{"Wait, what is it?"});
    CHECK(split("Wait . . . what is it?") == V{"Wait, what is it?"});
    CHECK(split("Wait\xE2\x80\xA6 what is it?") == V{"Wait, what is it?"});
    CHECK(split("He paused. . . . Then he left.") == V{"He paused.", "Then he left."});
    CHECK(split("He left...") == V{"He left."});
    CHECK(split("...and then") == V{"and then"});
    CHECK(split(". . .").empty());
    CHECK(split("Wait...What?") == V{"Wait, What?"});
    CHECK(split("A. B. C") == V{"A. B. C"});
}

TEST_CASE("tts segmenter ellipsis keeps highlight offsets on the source text")
{
    const std::string t = "Wait . . . what? Yes.";
    const auto s = eb::split_sentences(t);
    REQUIRE(s.size() == 2);
    CHECK(t.substr(s[0].begin, s[0].end - s[0].begin) == "Wait . . . what?");
    CHECK(t.substr(s[1].begin, s[1].end - s[1].begin) == "Yes.");
}

TEST_CASE("tts segmenter splits long comma sentences into short clause chunks")
{
    const std::string t = "When the old man finally reached the top of the hill, he sat down on a rock, looked at the "
                          "valley below, and thought about the long road behind him, which had taken years.";
    const auto s = eb::split_sentences(t);
    REQUIRE(s.size() >= 2);
    size_t prev_end = 0;
    for (const eb::Sentence& c : s) {
        CHECK(c.begin >= prev_end);
        CHECK(c.end - c.begin >= 25);
        prev_end = c.end;
    }
    CHECK(t.substr(s.front().begin, s.front().end - s.front().begin).starts_with("When the old man"));
    CHECK(t.substr(s.back().begin, s.back().end - s.back().begin).ends_with("years."));
    CHECK(split(t).front().ends_with(","));
}

TEST_CASE("tts segmenter leaves short sentences and decimal commas alone")
{
    CHECK(split("Short one, with a comma, but short.") == V{"Short one, with a comma, but short."});
    const std::string t = "The measured value was 1,5 and then 2,75 and then 3,25 in the first trial of the long experiment "
                          "that nobody wanted to repeat because it took far too many hours to finish.";
    CHECK(split(t) == V{t});
}

TEST_CASE("tts segmenter never makes a chunk shorter than the minimum")
{
    const std::string t = "Yes, no, ok, fine, sure, right, well, then, so, we went to the market to buy some fresh bread, "
                          "milk, eggs, butter, jam, honey, and a very long list of other things we needed that day.";
    const auto s = eb::split_sentences(t);
    for (const eb::Sentence& c : s)
        CHECK(c.end - c.begin >= 25);
}
