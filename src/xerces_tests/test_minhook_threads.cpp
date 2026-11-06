// MinHook as the client builds it: its thread walk for Freeze() covers only
// this process (src/xerces_deps/minhook/minhook_threads.inc, DESIGN.md 12.4).
// The hooks must still work while other threads run, and enabling one must no
// longer cost a snapshot of every thread on the PC.
//
// Timing on a PC that other builds keep busy: one enable can take longer
// than the walk itself while this process waits for a core. So the check
// takes the best of several enables, and measures the old per-enable cost (a
// snapshot of every thread on the PC) in the same run, under the same load.

#include "harness.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include <windows.h>

#include <tlhelp32.h>

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

double ms_since(std::chrono::steady_clock::time_point started) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - started)
      .count();
}

// What MinHook's own Freeze() did on every enable before Xerces' thread walk:
// a snapshot of every thread on the PC, walked to find this process's. It
// grows with the PC's thread count.
double system_thread_walk_ms() {
  const auto started = std::chrono::steady_clock::now();
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot != INVALID_HANDLE_VALUE) {
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    std::size_t mine = 0;
    if (Thread32First(snapshot, &entry)) {
      do {
        mine += entry.th32OwnerProcessID == GetCurrentProcessId();
        entry.dwSize = sizeof(entry);
      } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    static volatile std::size_t sink = 0;
    sink = mine;
  }
  return ms_since(started);
}
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

  // Enable and disable a few times: each one freezes the other threads. The
  // old cost, a walk of every thread on the PC, is measured between them.
  constexpr int rounds = 20;
  double best_ms = 1e9;
  double best_walk_ms = 1e9;
  for (int round = 0; round < rounds; ++round) {
    const auto started = std::chrono::steady_clock::now();
    REQUIRE(MH_EnableHook(reinterpret_cast<void *>(call_target)) == MH_OK);
    best_ms =
        (std::min)(best_ms, ms_since(started)); // windows.h has a min macro
    CHECK(call_target(4) == 50); // the detour ran, then the original
    REQUIRE(MH_DisableHook(reinterpret_cast<void *>(call_target)) == MH_OK);
    CHECK(call_target(4) == 5); // the original alone
    if (round % 5 == 0) {
      best_walk_ms = (std::min)(best_walk_ms, system_thread_walk_ms());
    }
  }
  CHECK(detour_calls.load() == rounds);
  CHECK(original_calls.load() == 2 * rounds);

  // The other threads were suspended and resumed, never left suspended.
  const long long before = spins.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(spins.load() > before);

  // A snapshot of every thread on the PC took 40-77 ms per enable on the
  // test PC; walking this process's 17 threads takes well under that. The
  // best enable is under 20 ms, or, on a PC so busy that even the best one is
  // not, under half of one walk of the PC's threads measured just now: an
  // enable that still walked them could not be.
  const bool fast = best_ms < 20.0 || best_ms * 2.0 < best_walk_ms;
  if (!fast) {
    std::printf("  minhook: best enable %.2f ms, a walk of the PC's threads "
                "%.2f ms\n",
                best_ms, best_walk_ms);
  }
  CHECK(fast);

  stop = true;
  for (std::thread &worker : workers) {
    worker.join();
  }
  CHECK(MH_RemoveHook(reinterpret_cast<void *>(call_target)) == MH_OK);
  CHECK(MH_Uninitialize() == MH_OK);
}
