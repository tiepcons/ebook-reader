#include "tts/azure/AzureProto.h"

#include <doctest/doctest.h>

#include <limits>

namespace {

constexpr const char* kVoicesSample = R"JSON([
  {"Name":"Microsoft Server Speech Text to Speech Voice (vi-VN, HoaiMyNeural)","DisplayName":"HoaiMy",
   "LocalName":"Hoài My","ShortName":"vi-VN-HoaiMyNeural","Gender":"Female","Locale":"vi-VN",
   "StyleList":["chat","cheerful"],"SampleRateHertz":"24000","VoiceType":"Neural","Status":"GA",
   "WordsPerMinute":"157","VoiceTag":{"TailoredScenarios":["Audiobook"],"ShortName":"decoy"}},
  {"LocalName":"Nam Minh","ShortName":"vi-VN-NamMinhNeural","Locale":"vi-VN","Gender":"Male"},
  {"LocalName":"Jenny","ShortName":"en-US-JennyNeural","Locale":"en-US"},
  {"LocalName":"Libby","ShortName":"en-GB-LibbyNeural","Locale":"en-GB"},
  {"LocalName":"Denise","ShortName":"fr-FR-DeniseNeural","Locale":"fr-FR"},
  {"LocalName":"Xiaoxiao","ShortName":"zh-CN-XiaoxiaoNeural","Locale":"zh-CN"}
])JSON";

}

TEST_CASE("tts azure region validation")
{
    CHECK(eb::valid_azure_region("southeastasia"));
    CHECK(eb::valid_azure_region("eu1"));
    CHECK_FALSE(eb::valid_azure_region("ab"));
    CHECK_FALSE(eb::valid_azure_region(""));
    CHECK_FALSE(eb::valid_azure_region("West"));
    CHECK_FALSE(eb::valid_azure_region("a.evil.com"));
    CHECK_FALSE(eb::valid_azure_region("a/b"));
    CHECK_FALSE(eb::valid_azure_region("a-b-c"));
    CHECK_FALSE(eb::valid_azure_region(std::string(33, 'a')));
    CHECK(eb::valid_azure_region(std::string(32, 'a')));
}

TEST_CASE("tts azure voice id validation")
{
    CHECK(eb::valid_azure_voice("vi-VN-HoaiMyNeural"));
    CHECK(eb::valid_azure_voice("en-US-AvaMultilingualNeural"));
    CHECK(eb::valid_azure_voice("zh-CN-liaoning-XiaobeiNeural"));
    CHECK_FALSE(eb::valid_azure_voice(""));
    CHECK_FALSE(eb::valid_azure_voice("HoaiMy"));
    CHECK_FALSE(eb::valid_azure_voice("vi-VN-"));
    CHECK_FALSE(eb::valid_azure_voice("vi-VN-a\"><x"));
    CHECK_FALSE(eb::valid_azure_voice("vi-VN-a b"));
    CHECK_FALSE(eb::valid_azure_voice(std::string("vi-VN-") + std::string(70, 'a')));
}

TEST_CASE("tts azure rate maps to a relative percentage")
{
    CHECK(eb::rate_percent(1.0) == 0);
    CHECK(eb::rate_percent(0.5) == -50);
    CHECK(eb::rate_percent(3.0) == 200);
    CHECK(eb::rate_percent(1.25) == 25);
    CHECK(eb::rate_percent(0.1) == -50);
    CHECK(eb::rate_percent(9.0) == 200);
    CHECK(eb::rate_percent(std::numeric_limits<double>::quiet_NaN()) == 0);
}

TEST_CASE("tts azure ssml escapes markup and builds the document")
{
    CHECK(eb::xml_escape("a & b < c > d \"e\" 'f'") == "a &amp; b &lt; c &gt; d &quot;e&quot; &apos;f&apos;");
    CHECK(eb::xml_escape("Vi\xE1\xBB\x87t") == "Vi\xE1\xBB\x87t");
    CHECK(eb::xml_escape("a\x01" "b\tc") == "a b\tc");
    CHECK(eb::xml_escape("a\xFF" "b") == "a\xEF\xBF\xBD" "b");
    CHECK(eb::xml_escape("\xED\xA0\x80").find('\xED') == std::string::npos);
    CHECK(eb::xml_escape("\xC0\x80") == "\xEF\xBF\xBD\xEF\xBF\xBD");

    const std::string ssml = eb::build_ssml("</prosody><x/>", "vi-VN-HoaiMyNeural", 25);
    CHECK(ssml
        == "<speak version=\"1.0\" xmlns=\"http://www.w3.org/2001/10/synthesis\" xml:lang=\"vi-VN\">"
           "<voice name=\"vi-VN-HoaiMyNeural\"><prosody rate=\"+25%\">&lt;/prosody&gt;&lt;x/&gt;</prosody></voice></speak>");
    CHECK(eb::build_ssml("x", "en-US-JennyNeural", -50).find("xml:lang=\"en-US\"") != std::string::npos);
    CHECK(eb::build_ssml("x", "en-US-JennyNeural", 0).find("rate=\"+0%\"") != std::string::npos);
    CHECK(eb::build_ssml("x", "bad\"voice", 0).empty());
}

TEST_CASE("tts azure voices json keeps vi-VN and en voices only")
{
    const auto v = eb::parse_voices(kVoicesSample, L"");
    REQUIRE(v.size() == 4);
    CHECK(v[0].id == L"vi-VN-HoaiMyNeural");
    CHECK(v[0].name == L"Hoài My");
    CHECK(v[0].lang == L"vi-VN");
    CHECK(v[1].id == L"vi-VN-NamMinhNeural");
    CHECK(v[2].id == L"en-US-JennyNeural");
    CHECK(v[3].id == L"en-GB-LibbyNeural");

    const auto picked = eb::parse_voices(kVoicesSample, L"fr-FR-DeniseNeural");
    REQUIRE(picked.size() == 5);
    CHECK(picked[4].id == L"fr-FR-DeniseNeural");
}

TEST_CASE("tts azure voices json rejects malformed and hostile input")
{
    CHECK(eb::parse_voices("", L"").empty());
    CHECK(eb::parse_voices("not json", L"").empty());
    CHECK(eb::parse_voices("[{\"ShortName\":\"vi-VN-A", L"").empty());
    CHECK(eb::parse_voices("[{\"ShortName\":\"vi-VN-Aaa\",\"Locale\":\"vi-VN\"}}]", L"").empty());
    CHECK(eb::parse_voices(std::string(200, '['), L"").empty());
    CHECK(eb::parse_voices("[{\"ShortName\":\"x\\q\"}]", L"").empty());
    CHECK(eb::parse_voices("[{\"ShortName\":\"vi-VN-A\\\"<\",\"Locale\":\"vi-VN\"}]", L"").empty());
    const auto surrogate = eb::parse_voices("[{\"ShortName\":\"vi-VN-Aaa\",\"Locale\":\"vi-VN\",\"LocalName\":\"\\ud83d\\ude00\"}]", L"");
    REQUIRE(surrogate.size() == 1);
    CHECK(surrogate[0].name == L"\U0001F600");
}
