// IN_SetActiveDevice's local client check (xerces_core/active_device.hpp).

#include "harness.hpp"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include <xerces_core/active_device.hpp>

using namespace xerces_core;

TEST_CASE("active device: only the engine's local clients reach the "
          "original") {
  // LOCAL_CLIENT_COUNT is 2: local clients 0 and 1.
  CHECK(local_client_in_range(0, 2));
  CHECK(local_client_in_range(1, 2));
  CHECK(!local_client_in_range(-1, 2)); // no primary local client yet
  CHECK(!local_client_in_range(2, 2));
  CHECK(!local_client_in_range(INT32_MIN, 2));
  CHECK(!local_client_in_range(INT32_MAX, 2));
  CHECK(!local_client_in_range(0, 0));
}

TEST_CASE("active device: the load's deferred call for client -1 is dropped "
          "and logged once") {
  // The 2026-11-05 17:13 crash: a mouse-button change on the game window
  // while it loaded, IN_MouseEvent on the window thread with the primary
  // local client still -1, upstream's hook deferring the call to the first
  // main frame. The check runs where the original would be called, so the
  // deferred call is dropped there.
  active_device_filter filter;
  CHECK(filter.check(-1, 2) == active_device_filter::verdict::drop_first);
  CHECK(filter.check(-1, 2) == active_device_filter::verdict::drop);
  CHECK(filter.check(2, 2) == active_device_filter::verdict::drop);
  CHECK_EQ(filter.dropped(), std::uint64_t{3});
  // Once the engine has its local client, calls go through, every time.
  CHECK(filter.check(0, 2) == active_device_filter::verdict::call);
  CHECK(filter.check(1, 2) == active_device_filter::verdict::call);
  CHECK(filter.check(0, 2) == active_device_filter::verdict::call);
  CHECK_EQ(filter.dropped(), std::uint64_t{3});
  // A later drop stays quiet.
  CHECK(filter.check(-1, 2) == active_device_filter::verdict::drop);
}

TEST_CASE("active device: the first drop is told once across threads") {
  active_device_filter filter;
  std::atomic<int> first{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < 1000; ++i) {
        if (filter.check(-1, 2) == active_device_filter::verdict::drop_first) {
          first.fetch_add(1);
        }
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  CHECK_EQ(first.load(), 1);
  CHECK_EQ(filter.dropped(), std::uint64_t{4000});
}
