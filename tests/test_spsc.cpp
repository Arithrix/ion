#include <doctest.h>

#include <thread>

#include "core/spsc_queue.h"

TEST_CASE("spsc queue keeps order and reports full") {
  ion::SpscQueue<int, 8> q;
  CHECK(q.empty());
  for (int i = 0; i < 7; ++i) CHECK(q.push(i));
  CHECK_FALSE(q.push(99));  // capacity - 1
  for (int i = 0; i < 7; ++i) {
    auto v = q.pop();
    REQUIRE(v);
    CHECK(*v == i);
  }
  CHECK_FALSE(q.pop());
}

TEST_CASE("spsc queue across two threads delivers every item once, in order") {
  ion::SpscQueue<uint32_t, 64> q;
  constexpr uint32_t kCount = 200000;
  std::thread producer([&] {
    for (uint32_t i = 0; i < kCount; ++i)
      while (!q.push(i)) std::this_thread::yield();
  });
  uint32_t expected = 0;
  while (expected < kCount) {
    if (auto v = q.pop()) {
      REQUIRE(*v == expected);
      ++expected;
    }
  }
  producer.join();
  CHECK(q.empty());
}
