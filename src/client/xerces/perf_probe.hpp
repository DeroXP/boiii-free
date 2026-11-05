#pragma once

// The frame probe (docs/DESIGN.md 12.2), on only when the game starts with
// XERCES_BENCH=1. download_overlay.cpp's Present hook (the only one) calls it
// three times: at entry, before the original Present and after it. When the
// probe is off that costs one relaxed atomic load and a branch per Present.
// When on, each Present of the game's swap chain becomes a 16-byte sample in
// a ring set aside at startup (no locks, no allocation on the Present path);
// a main-thread stamp per main frame goes into a second ring.
//
// Runs: console commands xerces_bench_begin [name], xerces_bench_end and
// xerces_bench_map <map> (the load time, from the command to the first main
// frame with the client active in the level, logged as perf.load), or
// XERCES_BENCH_AUTO=menu:<start_s>:<len_s> | ingame:<start_s>:<len_s>. A run
// writes <run>.frames.csv, <run>.main.csv and <run>.meta.json to
// XERCES_BENCH_OUT (default <home>\bench; never AppData, the game folder or
// the Workshop folder) from the async thread.
//
// perf.swapchain (always on) is logged once from the overlay's first Present.

#include <std_include.hpp>

#include <atomic>
#include <cstdint>

namespace xerces_perf {
namespace probe_detail {
extern std::atomic_bool armed;
} // namespace probe_detail

inline bool probe_armed() {
  return probe_detail::armed.load(std::memory_order_relaxed);
}

inline std::int64_t probe_clock() {
  LARGE_INTEGER value;
  QueryPerformanceCounter(&value);
  return value.QuadPart;
}

struct probe_entry {
  std::int64_t qpc = 0;
  std::uint32_t cpu = 0;
};

inline probe_entry probe_enter() {
  return {probe_clock(), GetCurrentProcessorNumber()};
}

// The overlay's first Present whose device it could read (render thread).
// Records the swap chain as the game's and logs perf.swapchain.
void on_first_present(IDXGISwapChain *chain, UINT sync_interval, UINT flags);

// After the original Present returned (render thread, probe armed only).
void probe_present(IDXGISwapChain *chain, const probe_entry &entry,
                   std::int64_t before, std::int64_t after, HRESULT result,
                   UINT sync_interval, bool overlay_drew);

// The thread that presents the game's swap chain (0 until seen).
std::uint32_t present_thread_id();
} // namespace xerces_perf
