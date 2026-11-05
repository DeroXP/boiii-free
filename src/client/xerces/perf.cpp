#include <std_include.hpp>

// xerces_perf (docs/DESIGN.md 12.2): startup marks, detour timing and the
// perf.io / perf.load_step records. See perf.hpp.

#include "perf.hpp"
#include "perf_probe.hpp"
#include "runtime.hpp"

#include <component/gpu.hpp>
#include <component/scheduler.hpp>
#include <game/game.hpp>
#include <loader/component_loader.hpp>

#include <utils/hook.hpp>

#include <xerces_core/frame_ring.hpp>
#include <xerces_core/json.hpp>
#include <xerces_core/perf_bench.hpp>

namespace xerces_perf {
namespace {
namespace bench = xerces_core::bench;
namespace json = xerces_core::json;

// How long perf.startup waits for the main menu before it logs without it
// (a game started straight into a level never shows it).
constexpr double menu_wait_ms = 180'000;

struct step_record {
  std::int64_t begin;
  std::int64_t end;
  std::uint64_t bytes;
  std::uint64_t items;
  const char *name;
  std::uint32_t thread;
  step_kind kind;
};

// Plain statics with no destructor work: the journal records its clean_exit
// write during shutdown, possibly during static destruction.
xerces_core::event_ring<step_record, 256> steps;

std::atomic<std::int64_t> ipc_start{0};
std::atomic<std::int64_t> frontend{0};
std::atomic<std::int64_t> menu_ready{0};
// The thread post_unpack ran on (the game's own start-up thread: main.cpp runs
// it from the game's SteamAPI_RestartAppIfNecessary call), and the thread
// perf.io labels "main": that one until the first main frame, then the main
// frame's. perf.startup logs both ids.
std::atomic<std::uint32_t> unpack_thread{0};
std::atomic<std::uint32_t> main_thread{0};

// Taken on the main thread at the first main frame and read on the async
// thread afterwards; the scheduler's lock orders the two.
utils::hook::detour_stats hooks_at_frontend{};
std::vector<utils::hook::detour_timeline_entry> timeline_at_frontend;
bool menu_timed_out = false;

std::string decimal(double value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.3f", value);
  return buffer;
}

// Milliseconds since this process was created, for a QPC stamp: one pair of
// (system time, QPC) read together anchors QPC to the creation time.
struct process_clock {
  bool valid = false;
  std::int64_t anchor_qpc = 0;
  double anchor_ms = 0;

  std::optional<double> ms(std::int64_t qpc) const {
    if (!valid || qpc == 0) {
      return std::nullopt;
    }
    return anchor_ms - bench::ticks_to_ms(anchor_qpc - qpc, qpc_frequency());
  }
};

const process_clock &startup_clock() {
  static const process_clock value = [] {
    process_clock out;
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel,
                         &user)) {
      return out;
    }
    FILETIME now{};
    GetSystemTimePreciseAsFileTime(&now);
    out.anchor_qpc = now_qpc();
    const auto ticks = [](const FILETIME &t) {
      return (std::int64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime;
    };
    out.anchor_ms = double(ticks(now) - ticks(creation)) / 10'000.0;
    out.valid = true;
    return out;
  }();
  return value;
}

std::string thread_label(std::uint32_t id) {
  if (id != 0 && id == main_thread.load(std::memory_order_relaxed)) {
    return "main";
  }
  if (id != 0 && id == present_thread_id()) {
    return "render";
  }
  return std::to_string(id);
}

// The async thread only.
void log_steps() {
  const auto lost = steps.drain([](const step_record &r) {
    const double ms = bench::ticks_to_ms(r.end - r.begin, qpc_frequency());
    const auto at = startup_clock().ms(r.begin);
    const std::string data =
        "{\"name\":" + json::quote(r.name) + ",\"ms\":" + decimal(ms) +
        ",\"at_ms\":" + (at ? decimal(*at) : "null") +
        ",\"qpc\":" + std::to_string(r.begin) +
        ",\"thread\":" + json::quote(thread_label(r.thread)) +
        ",\"bytes\":" + std::to_string(r.bytes) +
        ",\"items\":" + std::to_string(r.items) + "}";
    // perf.io or perf.load_step, the prefix apart (check_hosts.py would
    // read the whole first name as a host).
    const std::string event =
        std::string("perf.") + (r.kind == step_kind::io ? "io" : "load_step");
    xerces_runtime::log_info(
        event, std::string(r.name) + " " + decimal(ms) + " ms", data);
  });
  if (lost != 0) {
    xerces_runtime::log_warn("perf.lost",
                             std::to_string(lost) + " timing records lost",
                             "{\"lost\":" + std::to_string(lost) + "}");
  }
}

// The async thread, once.
void log_startup() {
  const process_clock &c = startup_clock();
  const auto hooks = hooks_at_frontend;
  bench::startup_marks marks;
  marks.first_detour_ms = c.ms(hooks.first_qpc);
  marks.last_detour_ms = c.ms(hooks.last_qpc);
  marks.ipc_start_ms = c.ms(ipc_start.load());
  marks.frontend_ms = c.ms(frontend.load());
  marks.menu_ready_ms = c.ms(menu_ready.load());
  marks.detours = hooks.created;
  marks.menu_timed_out = menu_timed_out;
  marks.unpack_thread_id = unpack_thread.load();
  marks.main_thread_id = main_thread.load();
  const auto seconds = [](const std::optional<double> &ms) {
    return ms ? decimal(*ms / 1000.0) + " s" : std::string("not seen");
  };
  xerces_runtime::log_info("perf.startup",
                           "first main frame " + seconds(marks.frontend_ms) +
                               ", menu " + seconds(marks.menu_ready_ms),
                           bench::startup_json(marks));

  const std::int64_t f = qpc_frequency();
  bench::hook_totals totals;
  totals.created = hooks.created;
  totals.enables = hooks.enables;
  totals.create_ms = bench::ticks_to_ms(hooks.create_ticks, f);
  totals.enable_ms = bench::ticks_to_ms(hooks.enable_ticks, f);
  totals.max_enable_ms = bench::ticks_to_ms(hooks.max_enable_ticks, f);
  const auto now = utils::hook::get_detour_stats();
  totals.created_later =
      now.created > hooks.created ? now.created - hooks.created : 0;
  for (const auto &entry : timeline_at_frontend) {
    totals.timeline.push_back(
        {c.ms(entry.qpc).value_or(0),
         bench::ticks_to_ms(entry.create_ticks, f) * 1000,
         bench::ticks_to_ms(entry.enable_ticks, f) * 1000});
  }
  xerces_runtime::log_info("perf.hooks",
                           std::to_string(totals.created) +
                               " detours: create " + decimal(totals.create_ms) +
                               " ms, enable " + decimal(totals.enable_ms) +
                               " ms, slowest enable " +
                               decimal(totals.max_enable_ms) + " ms",
                           bench::hooks_json(totals));
}

// The main thread, every 100 ms until the menu is ready.
bool poll_menu() {
  const std::int64_t now = now_qpc();
  if (gpu::on_main_menu()) {
    menu_ready.store(now);
  } else if (bench::ticks_to_ms(now - frontend.load(), qpc_frequency()) >
             menu_wait_ms) {
    menu_timed_out = true;
  } else {
    return scheduler::cond_continue;
  }
  scheduler::once(log_startup, scheduler::async);
  return scheduler::cond_end;
}

// The main thread, at the end of the first main-loop frame.
void first_main_frame() {
  frontend.store(now_qpc());
  main_thread.store(GetCurrentThreadId());
  hooks_at_frontend = utils::hook::get_detour_stats();
  timeline_at_frontend.resize(utils::hook::detour_timeline_capacity);
  timeline_at_frontend.resize(utils::hook::get_detour_timeline(
      timeline_at_frontend.data(), timeline_at_frontend.size()));
  scheduler::schedule(poll_menu, scheduler::main, 100ms);
}
} // namespace

std::int64_t now_qpc() {
  LARGE_INTEGER value{};
  QueryPerformanceCounter(&value);
  return value.QuadPart;
}

std::int64_t qpc_frequency() {
  static const std::int64_t frequency = [] {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return value.QuadPart > 0 ? value.QuadPart : std::int64_t{1};
  }();
  return frequency;
}

void record_step(step_kind kind, const char *name, std::int64_t begin_qpc,
                 std::int64_t end_qpc, std::uint64_t bytes,
                 std::uint64_t items) {
  steps.push(
      {begin_qpc, end_qpc, bytes, items, name, GetCurrentThreadId(), kind});
}

void note_ipc_start() { ipc_start.store(now_qpc()); }

std::int64_t menu_ready_qpc() {
  return menu_ready.load(std::memory_order_acquire);
}

std::uint32_t main_thread_id() {
  return main_thread.load(std::memory_order_relaxed);
}

class component final : public generic_component {
  DEFINE_COMPONENT_NAME("xerces_perf");

public:
  void post_unpack() override {
    if (!game::is_client() || !game::is_new_client()) {
      return;
    }
    (void)startup_clock(); // anchors QPC to the process's creation time early
    // Before the first drain below, so the journal writes and pool loads of
    // start-up are logged as "main" rather than a bare id.
    unpack_thread.store(GetCurrentThreadId());
    main_thread.store(GetCurrentThreadId());
    scheduler::once(first_main_frame, scheduler::main);
    scheduler::loop(log_steps, scheduler::async, 250ms);
  }
};
} // namespace xerces_perf

REGISTER_COMPONENT(xerces_perf::component)
