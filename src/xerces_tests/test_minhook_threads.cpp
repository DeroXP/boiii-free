// MinHook as the client builds it: its thread walk for Freeze() covers only
// this process (src/xerces_deps/minhook/minhook_threads.inc, DESIGN.md 12.4).
// The hooks must still work while other threads run, and enabling one must no
// longer cost a snapshot of every thread on the PC.

#include "harness.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <windows.h>

#include <MinHook.h>

namespace {
std::atomic<int> original_calls{0};
std::atomic<int> detour_calls{0};

using target_fn = int (*)(int);
target_fn trampoline = nullptr;

__declspec(noinline) int target(int value) {
  ++original_calls;
  return value + 1;
}

int detour(int value) {
  ++detour_calls;
  return trampoline(value) * 10;
}

// Keeps the call through a volatile pointer, so the compiler cannot inline
// target or fold the result.
target_fn volatile call_target = &target;
} // namespace

TEST_CASE("minhook: hooks work while threads run, and enabling is fast") {
  REQUIRE(MH_Initialize() == MH_OK);

  std::atomic<bool> stop{false};
  std::atomic<long long> spins{0};
  std::vector<std::thread> workers;
  for (int i = 0; i < 16; ++i) {
    workers.emplace_back([&] {
      while (!stop.load(std::memory_order_relaxed)) {
        spins.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::yield();
      }
    });
  }

  REQUIRE(MH_CreateHook(reinterpret_cast<void *>(call_target),
                        reinterpret_cast<void *>(&detour),
                        reinterpret_cast<void **>(&trampoline)) == MH_OK);

  // Enable and disable a few times: each one freezes the other threads.
  double slowest_ms = 0;
  for (int round = 0; round < 10; ++round) {
    const auto started = std::chrono::steady_clock::now();
    REQUIRE(MH_EnableHook(reinterpret_cast<void *>(call_target)) == MH_OK);
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - started)
                          .count();
    slowest_ms = ms > slowest_ms ? ms : slowest_ms;
    CHECK(call_target(4) == 50); // the detour ran, then the original
    REQUIRE(MH_DisableHook(reinterpret_cast<void *>(call_target)) == MH_OK);
    CHECK(call_target(4) == 5); // the original alone
  }
  CHECK(detour_calls.load() == 10);
  CHECK(original_calls.load() == 20);

  // The other threads were suspended and resumed, never left suspended.
  const long long before = spins.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(spins.load() > before);

  // A snapshot of every thread on the PC took 40-77 ms per enable on the
  // test PC; walking this process's 17 threads takes well under that.
  CHECK(slowest_ms < 20.0);

  stop = true;
  for (std::thread &worker : workers) {
    worker.join();
  }
  CHECK(MH_RemoveHook(reinterpret_cast<void *>(call_target)) == MH_OK);
  CHECK(MH_Uninitialize() == MH_OK);
}
