#include <std_include.hpp>

// xerces_stream_sort: the texture streamer's sort without the long waits
// (docs/DESIGN.md 12.4 item 7).
//
// The engine's streamer works in rounds. The last step of a round starts the
// job "r_stream_sort", which puts two lists in order: the image parts wanted
// and not loaded, and the image parts loaded. The next frame's streamer
// update waits for that job before it does anything else (it sleeps 0 ms in a
// loop until the job's count is 0).
//
// While the streamer takes the level to be still loading, the job sorts both
// lists whole, and when the list of wanted parts is as long as it was the
// last time it does so with an insertion sort, on the idea that they are
// almost in order. That sort takes a step for every pair out of order: 151 ms
// for a list of 13,800 loaded parts back to front. The main thread waits for
// all of it. The player's recordings of TOWN REIMAGINED with eight mods show
// such a frame, 125 to 143 ms, up to three times a second; what keeps their
// lists that far out of order is not known (docs/TESTING.md, "Town
// Reimagined's 140 ms frames").
//
// The two calls of that insertion sort are pointed at a merge of the
// stretches that are in order already (xerces_core/stream_sort.hpp), which
// asks the engine's own comparison (the one its std::sort uses for the same
// lists) which of two parts goes first. Equal parts keep their order, as with
// the insertion sort, so each list ends up exactly as the engine's sort would
// have left it; only the time differs.
//
// For tests: XERCES_STREAM_SORT=engine keeps the engine's sort and times it,
// XERCES_STREAM_SORT=off installs nothing. With XERCES_BENCH=1 or
// XERCES_STREAM_SORT_LOG=1 the log gets a line every 10 s with what the sorts
// of that time cost. XERCES_STREAM_SORT_TEST=loading keeps the streamer in
// the state it is in while a level loads, the only one in which it sorts its
// lists whole: every frame it is told that not everything is loaded and that
// its lists changed. XERCES_STREAM_SORT_TEST=reverse does the same and turns
// the list of loaded parts back to front before each sort, the worst case.

#include <component/scheduler.hpp>
#include <game/game.hpp>
#include <loader/component_loader.hpp>
#include <utils/hook.hpp>

#include <xerces_core/json.hpp>
#include <xerces_core/sigruntime.hpp>
#include <xerces_core/stream_sort.hpp>

#include "runtime.hpp"
#include "signatures.hpp"

namespace xerces_stream_sort {
namespace {
namespace core = xerces_core::stream_sort;
namespace ids = xerces_core::stream_sort_signatures;

// The engine's std::sort predicate for these lists: an empty object, the
// part compared, and the part it is compared with. True when the first goes
// before the second.
using before_fn = bool (*)(void *self, std::uint32_t a, std::uint32_t b);
// The engine's insertion sort, as its two calls pass things: the list's first
// entry, one past its last, and the (empty) predicate by value.
using insertion_fn = void (*)(std::uint32_t *first, std::uint32_t *last,
                              std::uint8_t order);

before_fn engine_before = nullptr;
insertion_fn engine_insertion = nullptr;
bool keep_engine_sort = false;
// XERCES_BENCH=1 or XERCES_STREAM_SORT_LOG=1: the log gets what the sorts cost
bool tally_on = false;
// XERCES_STREAM_SORT_TEST: the streamer's "lists changed" flag, and its
// "everything is loaded" flag 0x5E bytes before it (0x150AB271C and
// 0x150AB26BE in build 0x6531394)
bool test_reverse = false;
std::uint8_t *lists_changed = nullptr;
constexpr std::ptrdiff_t all_loaded_before = 0x5E;

// One job sorts at a time; the lock is for a second one that should never be
std::mutex work_mutex;
core::scratch work;

enum list : int { wanted = 0, loaded = 1, list_count };
constexpr std::array<const char *, list_count> list_name = {"wanted", "loaded"};

// What the sorts cost since the last log line. Written by the job's thread,
// read and cleared by the logging thread.
struct tally {
  std::atomic<std::uint32_t> sorts{0};
  std::atomic<std::uint64_t> total_us{0};
  std::atomic<std::uint32_t> longest_us{0};
  std::atomic<std::uint32_t> longest_items{0};
  std::atomic<std::uint32_t> longest_runs{0};
  std::atomic<std::uint32_t> most_items{0};
  std::atomic<std::uint32_t> most_runs{0};
};
std::array<tally, list_count> tallies;

// A list this long with this many stretches out of order is the case that
// cost the engine's sort tens of milliseconds. Counted, and said in the log
// at most once a minute: a tester's log then shows it happened and what it
// cost, with no switch set.
constexpr std::uint32_t far_items = 4096;
constexpr std::uint32_t far_runs = 512;
struct far_tally {
  std::atomic<std::uint32_t> sorts{0};
  std::atomic<std::uint32_t> longest_us{0};
  std::atomic<std::uint32_t> items{0};
  std::atomic<std::uint32_t> runs{0};
};
far_tally far_sorts;

std::int64_t ticks() {
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  return now.QuadPart;
}

std::int64_t ticks_per_second() {
  static const std::int64_t value = [] {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return f.QuadPart > 0 ? f.QuadPart : 1;
  }();
  return value;
}

void raise(std::atomic<std::uint32_t> &slot, const std::uint32_t value) {
  std::uint32_t seen = slot.load(std::memory_order_relaxed);
  while (value > seen &&
         !slot.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
  }
}

void note(const list which, const std::size_t items, const std::size_t runs,
          const std::int64_t took) {
  const auto us = static_cast<std::uint32_t>(std::min<std::int64_t>(
      took * 1'000'000 / ticks_per_second(), UINT32_MAX));
  const auto n =
      static_cast<std::uint32_t>(std::min<std::size_t>(items, UINT32_MAX));
  const auto r =
      static_cast<std::uint32_t>(std::min<std::size_t>(runs, UINT32_MAX));
  auto &t = tallies[which];
  t.sorts.fetch_add(1, std::memory_order_relaxed);
  t.total_us.fetch_add(us, std::memory_order_relaxed);
  if (us >= t.longest_us.load(std::memory_order_relaxed)) {
    t.longest_us.store(us, std::memory_order_relaxed);
    t.longest_items.store(n, std::memory_order_relaxed);
    t.longest_runs.store(r, std::memory_order_relaxed);
  }
  raise(t.most_items, n);
  raise(t.most_runs, r);
  if (n >= far_items && r >= far_runs) {
    far_sorts.sorts.fetch_add(1, std::memory_order_relaxed);
    if (us >= far_sorts.longest_us.load(std::memory_order_relaxed)) {
      far_sorts.longest_us.store(us, std::memory_order_relaxed);
      far_sorts.items.store(n, std::memory_order_relaxed);
      far_sorts.runs.store(r, std::memory_order_relaxed);
    }
  }
}

void sort_list(const list which, std::uint32_t *first, std::uint32_t *last,
               const std::uint8_t order) {
  if (last <= first) {
    return;
  }
  const auto items = static_cast<std::size_t>(last - first);
  std::size_t runs = 0;
  bool sorted = false;
  if (test_reverse && which == loaded) {
    std::reverse(first, last);
  }
  if (keep_engine_sort && tally_on) {
    // How far from in order the list is, for the tally: the engine's sort
    // does not say. Counted before the clock starts.
    std::uint8_t self = order;
    runs = 1;
    for (std::size_t i = 1; i < items; ++i) {
      runs += engine_before(&self, first[i], first[i - 1]) ? 1 : 0;
    }
  }
  const std::int64_t start = ticks();
  if (!keep_engine_sort) {
    try {
      const std::lock_guard lock(work_mutex);
      std::uint8_t self = order;
      const auto done = core::sort(
          std::span<std::uint32_t>(first, items),
          [&self](const std::uint32_t a, const std::uint32_t b) {
            return engine_before(&self, a, b);
          },
          work);
      runs = done.runs;
      sorted = true;
    } catch (const std::exception &) {
      // No memory for the merge: the engine's sort needs none
    }
  }
  if (!sorted) {
    engine_insertion(first, last, order);
  }
  note(which, items, runs, ticks() - start);
}

void wanted_stub(std::uint32_t *first, std::uint32_t *last,
                 const std::uint8_t order) {
  sort_list(wanted, first, last, order);
}

void loaded_stub(std::uint32_t *first, std::uint32_t *last,
                 const std::uint8_t order) {
  sort_list(loaded, first, last, order);
}

std::string env(const char *name) {
  char buffer[64];
  const DWORD size = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
  if (size == 0 || size >= sizeof(buffer)) {
    return {};
  }
  return std::string(buffer, size);
}

// One line with what the sorts cost since the last one; nothing when there
// were none
void log_tally() {
  std::string data = "{\"sort\":";
  data += keep_engine_sort ? "\"engine\"" : "\"merge\"";
  bool any = false;
  for (int i = 0; i < list_count; ++i) {
    auto &t = tallies[i];
    const std::uint32_t sorts = t.sorts.exchange(0);
    const std::uint64_t total = t.total_us.exchange(0);
    const std::uint32_t longest = t.longest_us.exchange(0);
    const std::uint32_t longest_items = t.longest_items.exchange(0);
    const std::uint32_t longest_runs = t.longest_runs.exchange(0);
    const std::uint32_t most_items = t.most_items.exchange(0);
    const std::uint32_t most_runs = t.most_runs.exchange(0);
    any = any || sorts > 0;
    data += std::string(",\"") + list_name[i] +
            "\":{\"sorts\":" + std::to_string(sorts) +
            ",\"total_us\":" + std::to_string(total) +
            ",\"longest_us\":" + std::to_string(longest) +
            ",\"longest_items\":" + std::to_string(longest_items) +
            ",\"longest_runs\":" + std::to_string(longest_runs) +
            ",\"most_items\":" + std::to_string(most_items) +
            ",\"most_runs\":" + std::to_string(most_runs) + "}";
  }
  data += "}";
  if (any) {
    xerces_runtime::log_info("xerces.stream_sort.tally",
                             "The texture streamer's sorts of the last 10 s",
                             data);
  }
}

// The sorts of the last minute whose list was far out of order; nothing when
// there were none
void log_far() {
  const std::uint32_t sorts = far_sorts.sorts.exchange(0);
  const std::uint32_t longest = far_sorts.longest_us.exchange(0);
  const std::uint32_t items = far_sorts.items.exchange(0);
  const std::uint32_t runs = far_sorts.runs.exchange(0);
  if (sorts == 0) {
    return;
  }
  xerces_runtime::log_info(
      "xerces.stream_sort.far",
      "The texture streamer's lists were far out of order " +
          std::to_string(sorts) +
          " times in the last minute; the longest "
          "sort took " +
          std::to_string(longest / 1000) + "." +
          std::to_string(longest % 1000 / 100) + " ms",
      std::string("{\"sort\":") +
          (keep_engine_sort ? "\"engine\"" : "\"merge\"") + ",\"sorts\":" +
          std::to_string(sorts) + ",\"longest_us\":" + std::to_string(longest) +
          ",\"items\":" + std::to_string(items) +
          ",\"runs\":" + std::to_string(runs) + "}");
}

bool bytes_at(const std::uintptr_t at, std::initializer_list<std::uint8_t> b) {
  return std::memcmp(reinterpret_cast<const void *>(at), std::data(b),
                     b.size()) == 0;
}

std::uintptr_t call_target(const std::uintptr_t call) {
  std::int32_t rel = 0;
  std::memcpy(&rel, reinterpret_cast<const void *>(call + 1), sizeof(rel));
  return call + 5 + rel;
}

void log_off(const std::string &message) {
  xerces_runtime::log_warn("xerces.feature.off", message,
                           R"({"feature":"stream_sort"})");
}

void install() {
  const std::string mode = env("XERCES_STREAM_SORT");
  if (mode == "off") {
    xerces_runtime::log_info("xerces.stream_sort.off",
                             "The texture streamer's sort is left alone "
                             "(XERCES_STREAM_SORT=off)");
    return;
  }
  keep_engine_sort = mode == "engine";
  tally_on = env("XERCES_BENCH") == "1" || env("XERCES_STREAM_SORT_LOG") == "1";

  if (!xerces_signatures::feature_enabled(core::feature)) {
    log_off(xerces_signatures::feature_message(
        core::feature, "The texture streamer's quicker sort",
        "The game's own sort stays: large maps with many mods can show a "
        "long frame every few frames."));
    return;
  }
  const auto wanted_at = xerces_signatures::address(ids::wanted_insertion_call);
  const auto loaded_at = xerces_signatures::address(ids::loaded_insertion_call);
  const auto before_at = xerces_signatures::address(ids::compare);
  if (!wanted_at || !loaded_at || !before_at) {
    log_off("The texture streamer's quicker sort is off: the signature "
            "registry has no address for its calls or its comparison");
    return;
  }
  // Both calls must be calls, to one function, which starts as the engine's
  // insertion sort does (it keeps its second argument and three registers);
  // the comparison is checked against its whole pattern again.
  std::string why;
  if (!bytes_at(*wanted_at, {0xE8}) || !bytes_at(*loaded_at, {0xE8}) ||
      call_target(*wanted_at) != call_target(*loaded_at) ||
      !bytes_at(call_target(*wanted_at),
                {0x48, 0x89, 0x54, 0x24, 0x10, 0x56, 0x41, 0x54, 0x41, 0x56})) {
    log_off("The texture streamer's quicker sort is off: the calls of the "
            "game's sort are not as expected");
    return;
  }
  if (!xerces_signatures::code_matches(
          ids::compare,
          std::array<std::uint8_t, 5>{0x48, 0x83, 0xEC, 0x08, 0xF3}, &why)) {
    log_off("The texture streamer's quicker sort is off: " + why);
    return;
  }
  engine_insertion = reinterpret_cast<insertion_fn>(call_target(*wanted_at));
  engine_before = reinterpret_cast<before_fn>(*before_at);
  try {
    utils::hook::call(*wanted_at, &wanted_stub);
    utils::hook::call(*loaded_at, &loaded_stub);
  } catch (const std::exception &e) {
    log_off(std::string("The texture streamer's quicker sort is off: its "
                        "calls could not be changed (") +
            e.what() + ")");
    return;
  }
  xerces_runtime::log_info(
      "xerces.feature.on",
      keep_engine_sort
          ? "The texture streamer's sort is timed, the game's own kept "
            "(XERCES_STREAM_SORT=engine)"
          : "The texture streamer's lists are sorted by merging",
      R"({"feature":"stream_sort"})");

  scheduler::loop(log_far, scheduler::async, 60s);
  if (tally_on) {
    scheduler::loop(log_tally, scheduler::async, 10s);
  }
  const std::string test = env("XERCES_STREAM_SORT_TEST");
  if (test == "loading" || test == "reverse") {
    // The flag the job clears once both lists are sorted: mov byte ptr
    // [rip+X], 0, after the third list's sort (0x26 past the second call)
    const std::uintptr_t clear = *loaded_at + 0x26;
    if (bytes_at(clear, {0xC6, 0x05}) && bytes_at(clear + 6, {0x00})) {
      std::int32_t disp = 0;
      std::memcpy(&disp, reinterpret_cast<const void *>(clear + 2),
                  sizeof(disp));
      lists_changed = reinterpret_cast<std::uint8_t *>(clear + 7 + disp);
      test_reverse = test == "reverse";
      scheduler::loop(
          [] {
            *(lists_changed - all_loaded_before) = 0;
            *lists_changed = 1;
          },
          scheduler::main);
      xerces_runtime::log_warn(
          "xerces.stream_sort.test",
          test_reverse
              ? "Test: the streamer is kept in its loading state and the "
                "list of loaded image parts is reversed before every sort "
                "(XERCES_STREAM_SORT_TEST=reverse)"
              : "Test: the streamer is kept in its loading state, so it "
                "sorts its lists whole (XERCES_STREAM_SORT_TEST=loading)");
    } else {
      xerces_runtime::log_warn("xerces.stream_sort.test",
                               "XERCES_STREAM_SORT_TEST: the streamer's flag "
                               "was not found, nothing done");
    }
  }
}
} // namespace

class component final : public generic_component {
  DEFINE_COMPONENT_NAME("xerces_stream_sort");

public:
  void post_unpack() override {
    if (!game::is_client() || !game::is_new_client()) {
      return;
    }
    install();
  }
};
} // namespace xerces_stream_sort

REGISTER_COMPONENT(xerces_stream_sort::component)
