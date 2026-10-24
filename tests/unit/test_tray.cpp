#include <doctest/doctest.h>

#include "lib/Schema.h"
#include "test_base_tmp.h"
#include "ui/CloseAsk.h"
#include "ui/SingleInstance.h"
#include "ui/Tray.h"

#include <shellapi.h>

#include <string>
#include <vector>

TEST_CASE("tray items follow the TTS state")
{
    using eb::TtsState;
    auto off = eb::tray_items(TtsState::Unavailable);
    CHECK_FALSE(off.play_pause);
    CHECK_FALSE(off.next);
    CHECK_FALSE(off.stop);
    auto idle = eb::tray_items(TtsState::Idle);
    CHECK(idle.play_pause);
    CHECK_FALSE(idle.stop);
    CHECK(std::wstring(idle.play_label) == L"Đọc to");
    auto playing = eb::tray_items(TtsState::Playing);
    CHECK(playing.next);
    CHECK(playing.stop);
    CHECK(std::wstring(playing.play_label) == L"Tạm dừng");
    CHECK(std::wstring(eb::tray_items(TtsState::Paused).play_label) == L"Đọc to");
}

TEST_CASE("tray tooltip carries the title and respects the shell limit")
{
    CHECK(eb::tray_tooltip(L"") == L"Ebook Reader");
    CHECK(eb::tray_tooltip(L"Dune") == L"Ebook Reader \u2014 Dune");
    CHECK(eb::tray_tooltip(std::wstring(500, L'x')).size() == 127);
}

TEST_CASE("tray callback decoding follows NOTIFYICON_VERSION_4")
{
    CHECK(eb::tray_action(MAKELPARAM(NIN_SELECT, 1)) == eb::TrayAction::Restore);
    CHECK(eb::tray_action(MAKELPARAM(NIN_KEYSELECT, 1)) == eb::TrayAction::Restore);
    CHECK(eb::tray_action(MAKELPARAM(WM_LBUTTONDBLCLK, 1)) == eb::TrayAction::Restore);
    CHECK(eb::tray_action(MAKELPARAM(WM_CONTEXTMENU, 1)) == eb::TrayAction::Menu);
    CHECK(eb::tray_action(MAKELPARAM(WM_MOUSEMOVE, 1)) == eb::TrayAction::None);
}

TEST_CASE("close action parsing defaults to ask")
{
    using eb::CloseAction;
    CHECK(eb::parse_close_action("exit") == CloseAction::Exit);
    CHECK(eb::parse_close_action("tray") == CloseAction::Tray);
    CHECK(eb::parse_close_action("ask") == CloseAction::Ask);
    CHECK(eb::parse_close_action("garbage") == CloseAction::Ask);
    CHECK(eb::parse_close_action("") == CloseAction::Ask);
}

TEST_CASE("close dialog answers map buttons and the remember box")
{
    using eb::CloseChoice;
    const auto exit_kept = eb::close_answer(eb::kButtonExit, true);
    CHECK(exit_kept.choice == CloseChoice::Exit);
    CHECK(eb::remembered_action(exit_kept) == eb::CloseAction::Exit);
    const auto tray_once = eb::close_answer(eb::kButtonTray, false);
    CHECK(tray_once.choice == CloseChoice::Tray);
    CHECK_FALSE(eb::remembered_action(tray_once).has_value());
    CHECK(eb::remembered_action(eb::close_answer(eb::kButtonTray, true)) == eb::CloseAction::Tray);
    const auto cancelled = eb::close_answer(IDCANCEL, true);
    CHECK(cancelled.choice == CloseChoice::Cancel);
    CHECK_FALSE(eb::remembered_action(cancelled).has_value());
    CHECK(eb::close_answer(0, true).choice == CloseChoice::Cancel);
}

TEST_CASE("close action is stored in kv")
{
    ebtest::TempDir dir;
    auto db = eb::Db::open(dir.path() / L"k.db");
    REQUIRE(db.has_value());
    REQUIRE(eb::migrate(*db).has_value());
    CHECK(eb::load_close_action(*db) == eb::CloseAction::Ask);
    CHECK(eb::store_close_action(*db, eb::CloseAction::Tray));
    CHECK(eb::load_close_action(*db) == eb::CloseAction::Tray);
    CHECK(eb::store_close_action(*db, eb::CloseAction::Exit));
    CHECK(eb::load_close_action(*db) == eb::CloseAction::Exit);
}

TEST_CASE("launch arguments: first plain argument is the file, switches are skipped")
{
    const std::vector<std::wstring_view> a{L"--smoke", L"--smoke-open", L"x.epub", L"--smoke-hold", L"5000", L"C:\\books\\a.epub"};
    const auto r = eb::parse_launch_args(a);
    CHECK(r.hold_ms == 5000);
    CHECK_FALSE(r.smoke_tray);
    CHECK(r.path == L"C:\\books\\a.epub");
    const std::vector<std::wstring_view> t{L"--smoke-tray"};
    const auto rt = eb::parse_launch_args(t);
    CHECK(rt.smoke_tray);
    CHECK(rt.path.empty());
    const std::vector<std::wstring_view> rel{L"a.epub"};
    CHECK(std::filesystem::path(eb::parse_launch_args(rel).path).is_absolute());
    const std::vector<std::wstring_view> bad{L"--smoke-hold", L"9x"};
    CHECK(eb::parse_launch_args(bad).hold_ms == 0);
    const std::vector<std::wstring_view> big{L"--smoke-hold", L"99999999999999"};
    CHECK(eb::parse_launch_args(big).hold_ms <= 60000);
}

TEST_CASE("instance key is per directory and case-insensitive")
{
    ebtest::TempDir dir;
    CHECK(eb::instance_key(dir.path()) == eb::instance_key(dir.path()));
    CHECK(eb::instance_key(dir.path()) != eb::instance_key(dir.path() / L"sub"));
    std::wstring upper = dir.path().wstring();
    for (auto& c : upper)
        c = static_cast<wchar_t>(towupper(c));
    CHECK(eb::instance_key(upper) == eb::instance_key(dir.path()));
    CHECK(eb::mutex_name(0xABCULL) == L"Local\\EbookReader-0000000000000abc");
}

TEST_CASE("copydata open requests are validated")
{
    const std::wstring ok = L"C:\\a.epub";
    const DWORD n = static_cast<DWORD>((ok.size() + 1) * sizeof(wchar_t));
    CHECK(eb::decode_open_request(eb::kCopyDataOpen, ok.c_str(), n) == ok);
    CHECK(eb::decode_open_request(eb::kCopyDataOpen, L"", sizeof(wchar_t)) == std::wstring{});
    CHECK_FALSE(eb::decode_open_request(eb::kCopyDataOpen + 1, ok.c_str(), n).has_value());
    CHECK_FALSE(eb::decode_open_request(eb::kCopyDataOpen, nullptr, n).has_value());
    CHECK_FALSE(eb::decode_open_request(eb::kCopyDataOpen, ok.c_str(), 0).has_value());
    CHECK_FALSE(eb::decode_open_request(eb::kCopyDataOpen, ok.c_str(), n - sizeof(wchar_t)).has_value());
    CHECK_FALSE(eb::decode_open_request(eb::kCopyDataOpen, ok.c_str(), n - 1).has_value());
    const std::wstring big(eb::kMaxCopyDataBytes, L'a');
    CHECK_FALSE(eb::decode_open_request(eb::kCopyDataOpen, big.c_str(), eb::kMaxCopyDataBytes + 2).has_value());
}

TEST_CASE("only the first instance per key owns the mutex")
{
    const std::uint64_t key = eb::instance_key(L"C:\\ebtest-single-instance-key");
    const eb::SingleInstance first(key);
    const eb::SingleInstance second(key);
    CHECK(first.is_first());
    CHECK_FALSE(second.is_first());
    const eb::SingleInstance other(key + 1);
    CHECK(other.is_first());
}
