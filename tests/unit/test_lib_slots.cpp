#include <doctest/doctest.h>

#include "lib/SlotLru.h"

TEST_CASE("slot lru: fills free slots in order, then reuses its own")
{
    eb::SlotLru lru(3, 8);
    CHECK(lru.acquire("a") == 8);
    CHECK(lru.acquire("b") == 9);
    CHECK(lru.acquire("c") == 10);
    CHECK(lru.acquire("a") == 8);
    CHECK(lru.size() == 3);
    CHECK(lru.contains("b"));
}

TEST_CASE("slot lru: never evicts an entry used in the current generation")
{
    eb::SlotLru lru(2, 0);
    lru.acquire("a");
    lru.acquire("b");
    CHECK(lru.saturated());
    CHECK_FALSE(lru.acquire("c").has_value());
    CHECK(lru.contains("a"));
    CHECK(lru.contains("b"));

    lru.begin_generation();
    CHECK_FALSE(lru.saturated());
    CHECK(lru.touch("b") == 1);
    CHECK(lru.acquire("c") == 0);
    CHECK_FALSE(lru.contains("a"));
    CHECK(lru.contains("b"));
    CHECK(lru.saturated());
    CHECK_FALSE(lru.acquire("d").has_value());
}

TEST_CASE("slot lru: evicts the least recently used unused entry")
{
    eb::SlotLru lru(3, 0);
    lru.acquire("a");
    lru.acquire("b");
    lru.acquire("c");
    lru.begin_generation();
    lru.touch("a");
    CHECK(lru.acquire("d") == 1);
    CHECK(lru.contains("a"));
    CHECK_FALSE(lru.contains("b"));
    CHECK(lru.contains("c"));
}

TEST_CASE("slot lru: zero capacity and reset")
{
    eb::SlotLru none(0, 0);
    CHECK_FALSE(none.acquire("a").has_value());
    CHECK(none.saturated());
    eb::SlotLru lru(1, 5);
    lru.acquire("a");
    lru.reset(2, 7);
    CHECK_FALSE(lru.contains("a"));
    CHECK(lru.acquire("b") == 7);
}
