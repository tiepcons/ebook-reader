#include <doctest/doctest.h>

#include "ui/ReaderSpread.h"

TEST_CASE("spread: decided by width and aspect")
{
    CHECK_FALSE(eb::wants_spread(700, 900, 96));
    CHECK_FALSE(eb::wants_spread(840, 500, 96));
    CHECK(eb::wants_spread(848, 700, 96));
    CHECK_FALSE(eb::wants_spread(1000, 900, 96));
    CHECK(eb::wants_spread(1000, 800, 96));
    CHECK(eb::wants_spread(1600, 1000, 96));
    CHECK_FALSE(eb::wants_spread(0, 0, 96));
}

TEST_CASE("spread: thresholds are in DIP, not pixels")
{
    CHECK_FALSE(eb::wants_spread(1000, 800, 192));
    CHECK(eb::wants_spread(2000, 1600, 192));
    CHECK(eb::spread_gutter_px(192) == 2 * eb::spread_gutter_px(96));
}

TEST_CASE("spread: slot width")
{
    CHECK(eb::slot_width_px(900, 96, 1) == 900);
    CHECK(eb::slot_width_px(908, 96, 2) == 450);
    CHECK(eb::slot_width_px(3, 96, 2) == 0);
}

TEST_CASE("spread: pairing is (2k, 2k+1)")
{
    CHECK(eb::spread_start(0, 10, 2) == 0);
    CHECK(eb::spread_start(1, 10, 2) == 0);
    CHECK(eb::spread_start(2, 10, 2) == 2);
    CHECK(eb::spread_start(9, 10, 2) == 8);
    CHECK(eb::spread_start(99, 10, 2) == 8);
    CHECK(eb::spread_start(-5, 10, 2) == 0);
    CHECK(eb::spread_start(7, 10, 1) == 7);
    CHECK(eb::spread_start(3, 0, 2) == 0);
}

TEST_CASE("spread: the right slot is empty after the last page")
{
    CHECK(eb::has_right_page(0, 10, 2));
    CHECK(eb::has_right_page(8, 10, 2));
    CHECK_FALSE(eb::has_right_page(8, 9, 2));
    CHECK_FALSE(eb::has_right_page(0, 10, 1));
    CHECK_FALSE(eb::has_right_page(0, 0, 2));
}

TEST_CASE("spread: navigation moves by one spread and stays inside the book")
{
    CHECK(eb::spread_move(0, 10, 2, 1) == 2);
    CHECK(eb::spread_move(2, 10, 2, -1) == 0);
    CHECK(eb::spread_move(0, 10, 2, -1) == 0);
    CHECK(eb::spread_move(8, 10, 2, 1) == 8);
    CHECK(eb::spread_move(6, 9, 2, 1) == 8);
    CHECK(eb::spread_move(8, 9, 2, 1) == 8);
    CHECK(eb::spread_move(4, 10, 1, 1) == 5);
    CHECK(eb::spread_move(0, 10, 1, -1) == 0);
    CHECK(eb::spread_move(3, 10, 2, -1) == 0);
}

TEST_CASE("spread: layout places two pages with a gutter, an empty slot keeps the left size")
{
    const eb::SpreadLayout one = eb::spread_layout(300, 500, 0, 0, 8, false);
    CHECK(one.content_w == 300);
    CHECK(one.content_h == 500);
    CHECK(one.right_x == 0);

    const eb::SpreadLayout two = eb::spread_layout(300, 500, 300, 500, 8, true);
    CHECK(two.content_w == 608);
    CHECK(two.right_x == 308);
    CHECK(two.left_y == 0);

    const eb::SpreadLayout empty = eb::spread_layout(300, 500, 0, 0, 8, true);
    CHECK(empty.content_w == 608);
    CHECK(empty.content_h == 500);

    const eb::SpreadLayout mixed = eb::spread_layout(300, 400, 250, 500, 8, true);
    CHECK(mixed.content_w == 558);
    CHECK(mixed.content_h == 500);
    CHECK(mixed.left_y == 50);
    CHECK(mixed.right_y == 0);
}

TEST_CASE("spread: default window is a portrait letter page, 85% of the work area high")
{
    const eb::WindowSize s = eb::default_window_size(1920, 1040);
    CHECK(s.h == 884);
    CHECK(s.w == doctest::Approx(884 * 8.5 / 11.0).epsilon(0.002));
    CHECK(s.w < s.h);
    const eb::WindowSize narrow = eb::default_window_size(500, 1040);
    CHECK(narrow.w == 500);
}
