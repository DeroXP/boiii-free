#pragma once

// xerces_perf (docs/DESIGN.md 12.2): the game role's always-on timing.
//
// - perf.startup and perf.hooks, logged once from the async thread when the
//   main menu is ready: milliseconds since the process was created for the
//   first and last detour, the command pipe, the first main frame and the
//   menu, and how long MinHook took (utils::hook::get_detour_stats).
// - perf.io and perf.load_step: a few known file writes and loads (config.cfg,
//   the journal, hash_names.txt, the Workshop pools) time themselves with
//   `timed`; the records go through a lock-free ring and are logged from the
//   async thread, never from the thread that did the work.
// - perf.swapchain comes from the frame probe (perf_probe.hpp).

#include <cstdint>

namespace xerces_perf {
std::int64_t now_qpc();
std::int64_t qpc_frequency();

enum class step_kind : std::uint8_t { io, load };

// Safe from any thread and during shutdown; `name` must be a string literal
// (or live as long as the process).
void record_step(step_kind kind, const char *name, std::int64_t begin_qpc,
                 std::int64_t end_qpc, std::uint64_t bytes = 0,
                 std::uint64_t items = 0);

// Times a scope and records it when the scope ends.
class timed {
public:
  timed(step_kind kind, const char *name)
      : kind_(kind), name_(name), begin_(now_qpc()) {}
  ~timed() { record_step(kind_, name_, begin_, now_qpc(), bytes_, items_); }
  timed(const timed &) = delete;
  timed &operator=(const timed &) = delete;

  void set_bytes(std::uint64_t bytes) { bytes_ = bytes; }
  void set_items(std::uint64_t items) { items_ = items; }

private:
  step_kind kind_;
  const char *name_;
  std::int64_t begin_;
  std::uint64_t bytes_ = 0;
  std::uint64_t items_ = 0;
};

// The command pipe is being served (ipc.cpp).
void note_ipc_start();

// When the main menu was first seen ready (0 until then), for the bench's
// XERCES_BENCH_AUTO=menu trigger. Readable from any thread.
std::int64_t menu_ready_qpc();
// The game's main thread: the thread post_unpack ran on until the first main
// frame, then the main frame's (0 before post_unpack).
std::uint32_t main_thread_id();
} // namespace xerces_perf
