#include <doctest/doctest.h>

#include "ui/ReaderLogic.h"

#include <atomic>
#include <cmath>
#include <stop_token>
#include <thread>

#include <windows.h>

namespace {

eb::RenderWant want(int page, uint32_t view = 1)
{
    eb::RenderWant w;
    w.page = page;
    w.view_id = view;
    return w;
}

}

TEST_CASE("reader: DIP to point layout math")
{
    const auto at96 = eb::reflow_layout(1000, 800, 96);
    CHECK(at96.width_pt == doctest::Approx((1000 - 48) * 0.75));
    CHECK(at96.height_pt == doctest::Approx((800 - 48) * 0.75));
    CHECK(at96.em_pt == doctest::Approx(12.0));
    const auto at192 = eb::reflow_layout(2000, 1600, 192);
    CHECK(at192.width_pt == doctest::Approx(at96.width_pt));
    CHECK(at192.em_pt == doctest::Approx(12.0));
    CHECK(eb::reflow_layout(1500, 900, 144).em_pt == doctest::Approx(12.0));
    CHECK(eb::reflow_layout(0, 0, 96).width_pt == doctest::Approx(eb::kMinLayoutPt));
    CHECK(eb::render_scale(96) == doctest::Approx(96.0 / 72.0));
}

TEST_CASE("reader: zoom clamp, fit modes and steps")
{
    CHECK(eb::clamp_zoom(10) == doctest::Approx(50));
    CHECK(eb::clamp_zoom(9999) == doctest::Approx(400));
    CHECK(eb::clamp_zoom(NAN) == doctest::Approx(100));
    const float actual = eb::render_scale(96);
    const eb::ZoomState page;
    CHECK(eb::fixed_scale(page, 600, 800, 1200, 800, 96, 16384) == doctest::Approx(1.0));
    const eb::ZoomState width{eb::ZoomState::Fit::Width, 100};
    CHECK(eb::fixed_scale(width, 600, 800, 900, 400, 96, 16384) == doctest::Approx(1.5));
    const eb::ZoomState custom{eb::ZoomState::Fit::Custom, 200};
    CHECK(eb::fixed_scale(custom, 600, 800, 100, 100, 96, 16384) == doctest::Approx(actual * 2));
    CHECK(eb::fixed_scale(eb::ZoomState{eb::ZoomState::Fit::Custom, 9000}, 100, 100, 100, 100, 96, 16384) == doctest::Approx(actual * 4));
    CHECK(eb::fixed_scale(page, 1, 1, 10000, 10000, 96, 16384) <= actual * 4 + 0.001F);
    CHECK(eb::fixed_scale(page, 0, 0, 100, 100, 96, 16384) == doctest::Approx(actual));
    CHECK(eb::limit_scale(5.0F, 1000, 1000, 2048) == doctest::Approx(2.047));
    CHECK(eb::limit_scale(5.0F, 4000, 4000, 16384) == doctest::Approx(std::sqrt(8.0 * 1024 * 1024 / 16.0e6)));
    CHECK(eb::limit_scale(1.0F, 100, 100, 4096) == doctest::Approx(1.0));
    const auto in = eb::zoom_step(actual, 96, 1);
    CHECK(in.fit == eb::ZoomState::Fit::Custom);
    CHECK(in.percent == doctest::Approx(125));
    CHECK(eb::zoom_step(actual * 4, 96, 1).percent == doctest::Approx(400));
    CHECK(eb::zoom_step(actual * 0.5F, 96, -1).percent == doctest::Approx(50));
    CHECK(eb::toggle_fit_width(page).fit == eb::ZoomState::Fit::Width);
    CHECK(eb::toggle_fit_width(width).fit == eb::ZoomState::Fit::Page);
}

TEST_CASE("reader: pan and placement")
{
    CHECK(eb::clamp_pan(-5, 1000, 400) == 0);
    CHECK(eb::clamp_pan(900, 1000, 400) == 600);
    CHECK(eb::clamp_pan(50, 300, 400) == 0);
    CHECK(eb::axis_origin(400, 300, 99) == 50);
    CHECK(eb::axis_origin(400, 1000, 100) == -100);
    CHECK(eb::bitmap_bytes(10, 20) == 800);
    CHECK(eb::bitmap_bytes(-1, 20) == 0);
}

TEST_CASE("reader: keys and clicks map to actions")
{
    using eb::NavAction;
    CHECK(eb::nav_for_key(VK_RIGHT, false, false) == NavAction::Next);
    CHECK(eb::nav_for_key(VK_NEXT, false, false) == NavAction::Next);
    CHECK(eb::nav_for_key(VK_LEFT, false, false) == NavAction::Prev);
    CHECK(eb::nav_for_key(VK_SPACE, false, false) == NavAction::Next);
    CHECK(eb::nav_for_key(VK_SPACE, true, false) == NavAction::Prev);
    CHECK(eb::nav_for_key(VK_HOME, false, false) == NavAction::First);
    CHECK(eb::nav_for_key(VK_END, false, false) == NavAction::Last);
    CHECK(eb::nav_for_key(VK_F11, false, false) == NavAction::Fullscreen);
    CHECK(eb::nav_for_key(VK_ESCAPE, false, false) == NavAction::Close);
    CHECK(eb::nav_for_key('W', false, false) == NavAction::ToggleFitWidth);
    CHECK(eb::nav_for_key(VK_OEM_PLUS, false, true) == NavAction::ZoomIn);
    CHECK(eb::nav_for_key(VK_OEM_MINUS, false, true) == NavAction::ZoomOut);
    CHECK(eb::nav_for_key(VK_RIGHT, false, true) == NavAction::None);
    CHECK(eb::nav_for_click(10, 400) == NavAction::Prev);
    CHECK(eb::nav_for_click(390, 400) == NavAction::Next);
    CHECK(eb::nav_for_click(200, 400) == NavAction::None);
    CHECK(eb::nav_for_click(-1, 400) == NavAction::None);
}

TEST_CASE("reader: LRU byte accounting")
{
    eb::ByteLru<int> lru(100);
    lru.put(1, 10, 40);
    lru.put(2, 20, 40);
    CHECK(lru.bytes() == 80);
    CHECK(lru.get(1) != nullptr); // 1 becomes most recent
    lru.put(3, 30, 40);           // over budget: evicts 2, the least recent
    CHECK(lru.bytes() == 80);
    CHECK_FALSE(lru.contains(2));
    CHECK(lru.contains(1));
    lru.put(1, 11, 10); // replacing frees the old bytes
    CHECK(lru.bytes() == 50);
    CHECK(*lru.get(1) == 11);
    lru.put(9, 90, 500); // a single oversized entry is kept alone
    CHECK(lru.size() == 1);
    CHECK(lru.bytes() == 500);
    lru.set_cap(120);
    CHECK(lru.size() == 1);
    lru.put(1, 1, 60);
    lru.put(2, 2, 60);
    lru.set_cap(60);
    CHECK(lru.size() == 1);
    CHECK(lru.contains(2));
    lru.clear();
    CHECK(lru.bytes() == 0);
    CHECK(lru.size() == 0);
}

TEST_CASE("reader: a newer request replaces the pending one and stale generations are detected")
{
    std::atomic<int> aborts{0};
    eb::RenderQueue q([&] { ++aborts; });
    const auto g1 = q.post(want(1));
    const auto g2 = q.post(want(2));
    CHECK(g2 > g1);
    CHECK(aborts == 0); // nothing was in flight
    eb::RenderWant w;
    const std::stop_source src;
    REQUIRE(q.pop(w, src.get_token()));
    CHECK(w.page == 2);
    CHECK(w.generation == g2);
    CHECK(q.current(g2));
    const auto g3 = q.post(want(3));
    CHECK(aborts == 1); // the render in flight is cancelled
    CHECK_FALSE(q.current(g2));
    CHECK(q.current(g3));
    q.done();
    q.post(want(4));
    CHECK(aborts == 1);
}

TEST_CASE("reader: supersede makes earlier requests stale and drops the pending one")
{
    eb::RenderQueue q([] {});
    const auto g = q.post(want(1));
    q.supersede();
    CHECK_FALSE(q.current(g));
    std::stop_source src;
    src.request_stop();
    eb::RenderWant w;
    CHECK_FALSE(q.pop(w, src.get_token()));
}

TEST_CASE("reader: pop returns on stop and wakes on a late post")
{
    eb::RenderQueue q([] {});
    std::stop_source src;
    src.request_stop();
    eb::RenderWant w;
    CHECK_FALSE(q.pop(w, src.get_token()));
    const std::stop_source live;
    const std::jthread poster([&] { q.post(want(7)); });
    REQUIRE(q.pop(w, live.get_token()));
    CHECK(w.page == 7);
}
