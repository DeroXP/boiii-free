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
// almost in order. That sort makes a move for every pair out of order: 151 ms
// for the 95 million pairs of a list of 13,800 loaded parts back to front.
// The main thread waits for all of it. The player's recordings of TOWN
// REIMAGINED with eight mods show such a frame, 125 to 143 ms, up to three
// times a second; what keeps their lists that far out of order is not known
// (docs/TESTING.md, "Town Reimagined's 140 ms frames").
//
// The two calls of that insertion sort are pointed at a merge of the
// stretches that are in order already (xerces_core/stream_sort.hpp), which
// asks the engine's own comparison (the one its std::sort uses for the same
// lists) which of two parts goes first. Each list ends up in the same order
// by that comparison. Parts that compare equal keep the order they had, where
// the engine's insertion sort puts a part it moves in front of its equals:
// the lists can differ in that and in nothing else, as they can after the
// engine's std::sort.
//
// The calls come on the engine's job threads, at any time until the process
// is gone. So everything they use is either a plain value or made once at
// start-up and never freed, they ask for no memory, take no lock that can
// fail, and throw nothing; when the merge cannot be used (another job is in
// it, which should never be) the engine's own sort does the work.
//
// xerces.stream_sort.far in the log says when a list was far out of order
// and what its sort cost: within 10 s the first time, at most once a minute
// after that, and once more when the game quits.
//
// The launch flag -xerces-stream-sort-engine (the launcher's Settings >
// Launch, Extra launch flags) keeps the engine's own sort, timed the same
// way: for telling whether a problem is the merge's.
//
// For tests: XERCES_STREAM_SORT=engine does the same as the flag,
// XERCES_STREAM_SORT=off installs nothing. With XERCES_BENCH=1 or
// XERCES_STREAM_SORT_LOG=1 the log gets a line every 10 s with what the sorts
// of that time cost. XERCES_STREAM_SORT_TEST=reverse turns the list of loaded
// parts back to front before each sort the game asks for, the worst case.

#include <component/scheduler.hpp>
#include <game/game.hpp>
#include <loader/component_loader.hpp>
#include <utils/hook.hpp>

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
// Both calls are changed
bool installed = false;
bool keep_engine_sort = false;
// XERCES_BENCH=1 or XERCES_STREAM_SORT_LOG=1: the log gets what the sorts cost
bool tally_on = false;
// XERCES_STREAM_SORT_TEST=reverse
bool test_reverse = false;

// A lock held for a few instructions. Nothing to construct or destroy, and
// taking it cannot fail.
class brief_lock {
public:
  void lock() noexcept {
    for (int spins = 0; held_.test_and_set(std::memory_order_acquire);
         ++spins) {
      if (spins < 64) {
        YieldProcessor();
      } else {
        SwitchToThread();
      }
    }
  }
  bool try_lock() noexcept {
    return !held_.test_and_set(std::memory_order_acquire);
  }
  void unlock() noexcept { held_.clear(std::memory_order_release); }

private:
  std::atomic_flag held_{};
};

// The merge's scratch, with room for the longest list there can be. Made at
// start-up and never freed: a job may be in the merge when the process ends.
core::scratch *work = nullptr;
// One job sorts at a time; a second one that should never be gets the
// engine's sort
brief_lock work_lock;

enum list : int { wanted = 0, loaded = 1, list_count };
constexpr std::array<const char *, list_count> list_name = {"wanted", "loaded"};

// A sort counts as far out of order when the engine's insertion sort would
// have had to make at least this many moves, about 6 ms of it (it took 151 ms
// for the 95 million of a list of 13,800 parts back to front). When the
// engine's sort did the work (it was asked for, or the merge could not be
// used) the moves are not known and its time decides.
constexpr std::uint64_t far_steps = 4'000'000;
constexpr std::uint32_t far_engine_us = 6'000;

// What the sorts cost since the last log line
struct list_tally {
  std::uint32_t sorts = 0;
  std::uint64_t total_us = 0;
  // The longest sort
  std::uint32_t longest_us = 0;
  std::uint32_t longest_items = 0;
  std::uint32_t longest_runs = 0;
  std::uint64_t longest_steps = 0;
  std::uint32_t most_items = 0;
  std::uint32_t most_runs = 0;
  std::uint64_t most_steps = 0;
};
// The sorts far out of order since the last log line about them, and the
// worst of them (by moves; by time when the engine's sort did the work)
struct far_tally {
  std::uint32_t sorts = 0;
  // How many of them the engine's sort did
  std::uint32_t by_engine = 0;
  std::uint64_t steps = 0;
  std::uint32_t items = 0;
  std::uint32_t runs = 0;
  std::uint32_t us = 0;
};
// Written by the job's thread, read and cleared by the logging thread, both
// under the lock
brief_lock tally_lock;
std::array<list_tally, list_count> tallies{};
far_tally far_sorts{};

std::int64_t ticks() {
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  return now.QuadPart;
}

// Both set once in install(), before the calls are changed
std::int64_t ticks_per_second = 1;
std::int64_t installed_at = 0;

std::uint32_t clamp32(const std::uint64_t value) {
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(value, UINT32_MAX));
}

void note(const list which, const std::size_t items, const std::size_t runs,
          const std::uint64_t steps, const bool by_engine,
          const std::int64_t took) noexcept {
  const std::uint32_t us =
      clamp32(static_cast<std::uint64_t>(std::max<std::int64_t>(took, 0)) *
              1'000'000 / static_cast<std::uint64_t>(ticks_per_second));
  const std::uint32_t n = clamp32(items);
  const std::uint32_t r = clamp32(runs);
  const bool far_off = by_engine ? us >= far_engine_us : steps >= far_steps;

  const std::lock_guard lock(tally_lock);
  auto &t = tallies[which];
  ++t.sorts;
  t.total_us += us;
  if (us >= t.longest_us) {
    t.longest_us = us;
    t.longest_items = n;
    t.longest_runs = r;
    t.longest_steps = steps;
  }
  t.most_items = std::max(t.most_items, n);
  t.most_runs = std::max(t.most_runs, r);
  t.most_steps = std::max(t.most_steps, steps);
  if (far_off) {
    ++far_sorts.sorts;
    far_sorts.by_engine += by_engine ? 1 : 0;
    const bool worst =
        by_engine ? us >= far_sorts.us : steps >= far_sorts.steps;
    if (worst) {
      far_sorts.steps = steps;
      far_sorts.items = n;
      far_sorts.runs = r;
      far_sorts.us = us;
    }
  }
}

void sort_list(const list which, std::uint32_t *first, std::uint32_t *last,
               const std::uint8_t order) noexcept {
  if (last <= first) {
    return;
  }
  const auto items = static_cast<std::size_t>(last - first);
  if (test_reverse && which == loaded) {
    std::reverse(first, last);
  }
  std::uint8_t self = order;
  std::size_t runs = 0;
  std::uint64_t steps = 0;
  if (keep_engine_sort && tally_on) {
    // How far from in order the list is, for the tally: the engine's sort
    // does not say. Counted before the clock starts.
    runs = 1;
    for (std::size_t i = 1; i < items; ++i) {
      runs += engine_before(&self, first[i], first[i - 1]) ? 1 : 0;
    }
  }
  const std::int64_t start = ticks();
  bool sorted = false;
  if (!keep_engine_sort && work_lock.try_lock()) {
    const auto done = core::sort(
        std::span<std::uint32_t>(first, items),
        [&self](const std::uint32_t a, const std::uint32_t b) {
          return engine_before(&self, a, b);
        },
        *work);
    work_lock.unlock();
    sorted = done.sorted;
    runs = done.runs;
    steps = done.steps;
  }
  if (!sorted) {
    engine_insertion(first, last, order);
  }
  note(which, items, runs, steps, !sorted, ticks() - start);
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

const char *sort_name() {
  return keep_engine_sort ? "\"engine\"" : "\"merge\"";
}

// One line with what the sorts cost since the last one; nothing when there
// were none
void log_tally() {
  std::array<list_tally, list_count> seen{};
  {
    const std::lock_guard lock(tally_lock);
    seen = tallies;
    tallies = {};
  }
  std::string data = std::string("{\"sort\":") + sort_name();
  bool any = false;
  for (int i = 0; i < list_count; ++i) {
    const auto &t = seen[i];
    any = any || t.sorts > 0;
    data += std::string(",\"") + list_name[i] +
            "\":{\"sorts\":" + std::to_string(t.sorts) +
            ",\"total_us\":" + std::to_string(t.total_us) +
            ",\"longest_us\":" + std::to_string(t.longest_us) +
            ",\"longest_items\":" + std::to_string(t.longest_items) +
            ",\"longest_runs\":" + std::to_string(t.longest_runs) +
            ",\"longest_steps\":" + std::to_string(t.longest_steps) +
            ",\"most_items\":" + std::to_string(t.most_items) +
            ",\"most_runs\":" + std::to_string(t.most_runs) +
            ",\"most_steps\":" + std::to_string(t.most_steps) + "}";
  }
  data += "}";
  if (any) {
    xerces_runtime::log_info("xerces.stream_sort.tally",
                             "The texture streamer's sorts since the last "
                             "such line",
                             data);
  }
}

std::string tenths_of_ms(const std::uint32_t us) {
  return std::to_string(us / 1000) + "." + std::to_string(us % 1000 / 100);
}

// The sorts whose list was far out of order since the last such line; nothing
// when there were none. Called every 10 s: the first line comes at once,
// later ones when a minute has passed, and what is left when the game quits
// (`quitting`).
std::int64_t far_logged_at = 0; // ticks; 0: no line yet
brief_lock far_log_lock;        // the 10 s loop and the quit can meet

void log_far(const bool quitting) {
  const std::lock_guard one_at_a_time(far_log_lock);
  const std::int64_t at = ticks();
  const std::int64_t since =
      (at - (far_logged_at != 0 ? far_logged_at : installed_at)) /
      ticks_per_second;
  if (!quitting && far_logged_at != 0 && since < 60) {
    return;
  }
  far_tally seen{};
  {
    const std::lock_guard lock(tally_lock);
    seen = far_sorts;
    far_sorts = {};
  }
  if (seen.sorts == 0) {
    return;
  }
  far_logged_at = at;
  const std::string times =
      std::to_string(seen.sorts) + (seen.sorts == 1 ? " time" : " times");
  const std::string message =
      seen.by_engine == seen.sorts
          ? "The texture streamer's lists took the game's own sort long " +
                times + ", at most " + tenths_of_ms(seen.us) + " ms (" +
                std::to_string(seen.items) + " parts)"
          : "The texture streamer's lists were far out of order " + times +
                "; the worst (" + std::to_string(seen.items) + " parts, " +
                std::to_string(seen.steps / 1'000'000) +
                " million moves or more for the game's own sort) was "
                "sorted in " +
                tenths_of_ms(seen.us) + " ms";
  xerces_runtime::log_info(
      "xerces.stream_sort.far", message,
      std::string("{\"sort\":") + sort_name() +
          ",\"sorts\":" + std::to_string(seen.sorts) +
          ",\"by_engine\":" + std::to_string(seen.by_engine) +
          ",\"steps\":" + std::to_string(seen.steps) +
          ",\"items\":" + std::to_string(seen.items) + ",\"runs\":" +
          std::to_string(seen.runs) + ",\"us\":" + std::to_string(seen.us) +
          ",\"seconds\":" + std::to_string(since) + "}");
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
  keep_engine_sort =
      mode == "engine" ||
      core::engine_sort_requested(xerces_runtime::command_line_args());
  tally_on = env("XERCES_BENCH") == "1" || env("XERCES_STREAM_SORT_LOG") == "1";

  if (!xerces_signatures::feature_enabled(core::feature)) {
    log_off(xerces_signatures::feature_message(
        core::feature, "The texture streamer's quicker sort",
        "The game's own sort stays: large maps with many mods can show a "
        "long frame several times a second."));
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
  // Everything the stubs use is ready before a call is changed
  LARGE_INTEGER frequency{};
  QueryPerformanceFrequency(&frequency);
  ticks_per_second = frequency.QuadPart > 0 ? frequency.QuadPart : 1;
  installed_at = ticks();
  try {
    auto scratch = std::make_unique<core::scratch>();
    scratch->reserve(core::most_items);
    work = scratch.release();
  } catch (const std::exception &e) {
    log_off(std::string("The texture streamer's quicker sort is off: no "
                        "memory for its work space (") +
            e.what() + ")");
    return;
  }
  test_reverse = env("XERCES_STREAM_SORT_TEST") == "reverse";
  const std::uintptr_t engine_sort = call_target(*wanted_at);
  engine_insertion = reinterpret_cast<insertion_fn>(engine_sort);
  engine_before = reinterpret_cast<before_fn>(*before_at);
  try {
    utils::hook::call(*wanted_at, &wanted_stub);
    try {
      utils::hook::call(*loaded_at, &loaded_stub);
    } catch (...) {
      // Both or neither: the first call goes back to the engine's sort
      utils::hook::call(*wanted_at, engine_sort);
      throw;
    }
  } catch (const std::exception &e) {
    log_off(std::string("The texture streamer's quicker sort is off: its "
                        "calls could not be changed (") +
            e.what() + ")");
    return;
  }
  installed = true;
  xerces_runtime::log_info(
      "xerces.feature.on",
      keep_engine_sort ? "The texture streamer's sort is the game's own, timed "
                         "(-xerces-stream-sort-engine)"
                       : "The texture streamer's lists are sorted by merging",
      R"({"feature":"stream_sort"})");
  if (test_reverse) {
    xerces_runtime::log_warn(
        "xerces.stream_sort.test",
        "Test: the list of loaded image parts is turned back to front before "
        "every sort (XERCES_STREAM_SORT_TEST=reverse)");
  }

  scheduler::loop([] { log_far(false); }, scheduler::async, 10s);
  if (tally_on) {
    scheduler::loop(log_tally, scheduler::async, 10s);
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

  // A quit from the game's menus comes through here (a crash and the
  // console's forced exit do not): what the lines have not said yet
  void pre_destroy() override {
    if (!installed) {
      return;
    }
    log_far(true);
    if (tally_on) {
      log_tally();
    }
  }
};
} // namespace xerces_stream_sort

REGISTER_COMPONENT(xerces_stream_sort::component)
