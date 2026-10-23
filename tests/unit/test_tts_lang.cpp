#include <doctest/doctest.h>

#include "tts/LangDetect.h"

#include <iterator>
#include <string>
#include <ostream>

using eb::Lang;

TEST_CASE("tts lang vietnamese letters")
{
    CHECK(eb::detect_sentence("Tôi đang đọc sách.", Lang::En) == Lang::Vi);
    CHECK(eb::detect_sentence("Hà Nội mùa thu", Lang::En) == Lang::Vi);
    CHECK(eb::detect_sentence("ĐƯỜNG VỀ NHÀ", Lang::En) == Lang::Vi);
    CHECK(eb::detect_sentence("Nguyễn Văn Ạ", Lang::En) == Lang::Vi);
}

TEST_CASE("tts lang decomposed vietnamese")
{
    CHECK(eb::detect_sentence("Nguye\xCC\x82\xCC\x83n Van", Lang::En) == Lang::Vi);
}

TEST_CASE("tts lang english")
{
    CHECK(eb::detect_sentence("The quick brown fox jumps over the lazy dog.", Lang::Vi) == Lang::En);
    CHECK(eb::detect_sentence("It was the best of times.", Lang::En) == Lang::En);
}

TEST_CASE("tts lang no letters takes book default")
{
    CHECK(eb::detect_sentence("1234 -- 56.7", Lang::Vi) == Lang::Vi);
    CHECK(eb::detect_sentence("1234 -- 56.7", Lang::En) == Lang::En);
    CHECK(eb::detect_sentence("", Lang::Vi) == Lang::Vi);
}

TEST_CASE("tts lang single vietnamese name keeps default")
{
    CHECK(eb::detect_sentence("Yesterday the minister Nguyễn visited Paris and London today.", Lang::En) == Lang::En);
    CHECK(eb::detect_sentence("Dr. Nguyễn arrived late.", Lang::En) == Lang::En);
}

TEST_CASE("tts lang book language from tag")
{
    CHECK(eb::book_language("vi", "") == Lang::Vi);
    CHECK(eb::book_language("vi-VN", "") == Lang::Vi);
    CHECK(eb::book_language("vie", "") == Lang::Vi);
    CHECK(eb::book_language("VI_vn", "") == Lang::Vi);
    CHECK(eb::book_language("en", "Tôi đi học") == Lang::En);
    CHECK(eb::book_language("en-US", "") == Lang::En);
    CHECK(eb::book_language("eng", "") == Lang::En);
}

TEST_CASE("tts lang book language from sample")
{
    CHECK(eb::book_language("", "Hôm nay trời đẹp. Tôi đi chơi cùng bạn bè. Chúng tôi rất vui.") == Lang::Vi);
    CHECK(eb::book_language("", "It was a bright cold day in April. The clocks were striking thirteen.") == Lang::En);
    CHECK(eb::book_language("", "") == Lang::En);
    CHECK(eb::book_language("fr", "Tôi đi học. Bạn đi đâu?") == Lang::Vi);
}

namespace {

struct Case {
    const char* text;
    Lang expect;
};

const Case kCases[] = {
    {"The weather today is wonderful and warm.", Lang::En},
    {"She walked to the market to buy some bread.", Lang::En},
    {"They have been waiting for the train since morning.", Lang::En},
    {"I met Nguyễn Văn An at the station yesterday.", Lang::En},
    {"The novel by Nguyễn Du is studied in schools.", Lang::En},
    {"We visited Hà Nội and then went to Huế.", Lang::En},
    {"Chapter 12: The return of the king.", Lang::En},
    {"What is the meaning of this?", Lang::En},
    {"He said that it was not his fault.", Lang::En},
    {"There is a book on the table by the window.", Lang::En},
    {"Programming is the art of telling a computer what to do.", Lang::En},
    {"She was born in Đà Nẵng but grew up in London.", Lang::En},
    {"All of the students passed the exam with ease.", Lang::En},
    {"It is a truth universally acknowledged.", Lang::En},
    {"The café on the corner serves excellent coffee and cake.", Lang::En},
    {"Phở is a popular dish that has been served for decades.", Lang::En},
    {"My brother and his wife live in Boston.", Lang::En},
    {"Call me Ishmael.", Lang::En},
    {"Do you know who has the key to the door?", Lang::En},
    {"Mr. Trần was the head of the committee for years.", Lang::En},
    {"Tôi yêu Việt Nam.", Lang::Vi},
    {"Hôm nay trời đẹp, chúng ta đi dạo nhé.", Lang::Vi},
    {"Cô ấy là giáo viên dạy toán ở trường làng.", Lang::Vi},
    {"Anh ta đi ra ngoài rồi.", Lang::Vi},
    {"Họ đã sống ở đây suốt ba mươi năm.", Lang::Vi},
    {"Tôi đang dùng laptop và email của công ty.", Lang::Vi},
    {"Chúng tôi mở Facebook để xem tin tức mỗi ngày.", Lang::Vi},
    {"Bạn có thể tải file về máy tính không?", Lang::Vi},
    {"Anh ấy làm việc ở một công ty phần mềm.", Lang::Vi},
    {"Trăm năm trong cõi người ta, chữ tài chữ mệnh khéo là ghét nhau.", Lang::Vi},
    {"Mẹ tôi nấu cơm mỗi buổi chiều.", Lang::Vi},
    {"Đây là một cuốn sách rất hay.", Lang::Vi},
    {"Người dân ở đó rất thân thiện và hiếu khách.", Lang::Vi},
    {"Thành phố Hồ Chí Minh là trung tâm kinh tế lớn nhất.", Lang::Vi},
    {"Em nhớ anh nhiều lắm.", Lang::Vi},
    {"Ông ấy đã đọc xong cuốn tiểu thuyết ấy.", Lang::Vi},
    {"Mùa xuân đến, hoa đào nở khắp nơi.", Lang::Vi},
    {"Hãy gửi email cho tôi trước thứ sáu.", Lang::Vi},
    {"Chương 3: Cuộc gặp gỡ bất ngờ", Lang::Vi},
    {"Họ chơi game online suốt đêm.", Lang::Vi},
    {"Cuộc họp bắt đầu lúc chín giờ sáng.", Lang::Vi},
    {"Nước Việt Nam có hơn chín mươi triệu dân.", Lang::Vi},
    {"Tôi muốn mua một chiếc điện thoại mới.", Lang::Vi},
    {"Cả nhà ngồi quanh mâm cơm ấm áp.", Lang::Vi},
    {"Nó chạy rất nhanh về phía cánh đồng.", Lang::Vi},
    {"Bài hát này làm tôi nhớ về quê hương.", Lang::Vi},
    {"Trời mưa to nên chúng tôi ở nhà.", Lang::Vi},
    {"Giá vé máy bay đã tăng gấp đôi.", Lang::Vi},
    {"Cô giáo khen em học giỏi.", Lang::Vi},
    {"The meeting starts at nine in the morning.", Lang::En},
    {"I would like to order a cup of tea, please.", Lang::En},
    {"When the rain stopped, we went outside to play.", Lang::En},
    {"Ông nói rằng việc này không dễ dàng chút nào.", Lang::Vi},
    {"This book was written by a famous author from Hải Phòng.", Lang::En},
    {"After dinner they sat by the fire and talked for hours.", Lang::En},
    {"Có một con mèo nhỏ đang ngủ trên ghế.", Lang::Vi},
    {"The company released a new version of the software last week.", Lang::En},
    {"Anh có khỏe không?", Lang::Vi},
    {"Please remember to lock the door before you leave.", Lang::En},
    {"Những đứa trẻ chạy nhảy trong sân trường.", Lang::Vi},
    {"Vietnamese coffee is strong, sweet and very popular.", Lang::En},
    {"Đà Lạt có khí hậu mát mẻ quanh năm.", Lang::Vi},
};

}

TEST_CASE("tts lang accuracy table")
{
    const size_t total = std::size(kCases);
    size_t ok = 0;
    for (const Case& c : kCases) {
        const bool good = eb::detect_sentence(c.text, Lang::En) == c.expect && eb::detect_sentence(c.text, Lang::Vi) == c.expect;
        if (!good)
            MESSAGE("misclassified: ", std::string(c.text));
        ok += good ? 1 : 0;
    }
    CHECK(total >= 60);
    CHECK(ok * 100 >= total * 95);
}
