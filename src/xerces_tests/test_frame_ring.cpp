// The frame probe's rings (DESIGN.md 12.2): one writer and a reader that
// copies a range out later; many writers and one reader for timing records.

#include "harness.hpp"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include <xerces_core/frame_ring.hpp>

using namespace xerces_core;

namespace {
// Two halves that must agree: a torn copy shows as a mismatch.
struct pair_item {
  std::uint64_t index;
  std::uint64_t check;
};
} // namespace

TEST_CASE("frame ring: copies a published range in order") {
  frame_ring<std::uint64_t> ring(4); // 16 items
  CHECK_EQ(ring.capacity(), 16u);
  for (std::uint64_t i = 0; i < 10; ++i) {
    ring.push(i * 3);
  }
  CHECK_EQ(ring.count(), 10u);
  std::vector<std::uint64_t> out;
  CHECK_EQ(ring.copy(2, 6, out), 2u);
  REQUIRE_EQ(out.size(), 4u);
  CHECK_EQ(out[0], 6u);
  CHECK_EQ(out[3], 15u);
  // The end is clamped to what was pushed.
  CHECK_EQ(ring.copy(8, 100, out), 8u);
  CHECK_EQ(out.size(), 2u);
  // An empty range copies nothing.
  CHECK_EQ(ring.copy(5, 5, out), 5u);
  CHECK(out.empty());
}

TEST_CASE("frame ring: a range the writer came round over starts later") {
  frame_ring<std::uint64_t> ring(3); // 8 items
  for (std::uint64_t i = 0; i < 20; ++i) {
    ring.push(i);
  }
  std::vector<std::uint64_t> out;
  // Index 12 is the oldest slot that no push can be rewriting (20 + 1 - 8 =
  // 13 is the first safe one: the next push, index 20, rewrites 12's slot).
  const std::uint64_t first = ring.copy(0, 20, out);
  CHECK_EQ(first, 13u);
  REQUIRE_EQ(out.size(), 7u);
  CHECK_EQ(out.front(), 13u);
  CHECK_EQ(out.back(), 19u);
  // A range wholly overwritten copies nothing.
  CHECK_EQ(ring.copy(0, 5, out), 5u);
  CHECK(out.empty());
}

TEST_CASE("frame ring: a reader never sees a torn item while the writer runs") {
  frame_ring<pair_item> ring(8); // 256 items, so the writer laps often
  std::atomic_bool stop{false};
  std::thread writer([&] {
    for (std::uint64_t i = 0; !stop.load(std::memory_order_relaxed); ++i) {
      ring.push({i, ~i});
      // A little work per item, so a reader can sometimes copy in time.
      for (volatile int spin = 0; spin < 64; spin = spin + 1) {
      }
    }
  });
  // The writer thread may take a while to start.
  while (ring.count() < 1000) {
    std::this_thread::yield();
  }
  std::vector<pair_item> out;
  std::uint64_t checked = 0;
  bool torn = false;
  bool order = true;
  for (int round = 0; round < 20000 && !torn; ++round) {
    const std::uint64_t end = ring.count();
    const std::uint64_t begin = end > 48 ? end - 48 : 0;
    const std::uint64_t first = ring.copy(begin, end, out);
    for (std::size_t k = 0; k < out.size(); ++k) {
      torn = torn || out[k].check != ~out[k].index;
      order = order && out[k].index == first + k;
    }
    checked += out.size();
  }
  stop = true;
  writer.join();
  CHECK(!torn);
  CHECK(order);
  CHECK(checked > 0u);
}

TEST_CASE("event ring: drains in push order and counts what it lost") {
  event_ring<std::uint64_t, 8> ring;
  for (std::uint64_t i = 0; i < 5; ++i) {
    ring.push(i);
  }
  std::vector<std::uint64_t> seen;
  CHECK_EQ(ring.drain([&](std::uint64_t v) { seen.push_back(v); }), 0u);
  REQUIRE_EQ(seen.size(), 5u);
  CHECK_EQ(seen[4], 4u);
  // Nothing new: nothing drained.
  seen.clear();
  CHECK_EQ(ring.drain([&](std::uint64_t v) { seen.push_back(v); }), 0u);
  CHECK(seen.empty());
  // 12 more into 8 slots: the reader lost the first 4 of them.
  for (std::uint64_t i = 5; i < 17; ++i) {
    ring.push(i);
  }
  CHECK_EQ(ring.drain([&](std::uint64_t v) { seen.push_back(v); }), 4u);
  REQUIRE_EQ(seen.size(), 8u);
  CHECK_EQ(seen.front(), 9u);
  CHECK_EQ(seen.back(), 16u);
}

TEST_CASE("event ring: several writers, every item arrives once") {
  static event_ring<pair_item, 4096> ring; // static, as the game holds it
  constexpr int writers = 4;
  constexpr std::uint64_t each = 500;
  std::atomic_int finished{0};
  std::vector<std::thread> threads;
  for (int w = 0; w < writers; ++w) {
    threads.emplace_back([w, &finished] {
      for (std::uint64_t i = 0; i < each; ++i) {
        const std::uint64_t value = std::uint64_t(w) * 1000000 + i;
        ring.push({value, ~value});
      }
      ++finished;
    });
  }
  std::vector<int> counts(writers, 0);
  bool torn = false;
  std::uint64_t lost = 0;
  const auto take = [&](const pair_item &item) {
    torn = torn || item.check != ~item.index;
    const auto w = static_cast<std::size_t>(item.index / 1000000);
    if (w < counts.size()) {
      ++counts[w];
    }
  };
  // Drains while the writers run, then once more after the last one ends.
  while (finished.load() < writers) {
    lost += ring.drain(take);
  }
  for (auto &t : threads) {
    t.join();
  }
  lost += ring.drain(take);
  CHECK(!torn);
  CHECK_EQ(lost, 0u);
  for (int w = 0; w < writers; ++w) {
    CHECK_EQ(counts[static_cast<std::size_t>(w)], int(each));
  }
}
