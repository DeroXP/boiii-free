#include <std_include.hpp>

// Present without the message loop's millisecond (docs/research/engine-re.md,
// Threads; docs/DESIGN.md 12.4). The game presents on its Window thread: the
// render backend sends the game's window WM_APP + 0x43 and waits, and the
// window's procedure calls IDXGISwapChain::Present (0x8042 creates the swap
// chain, 0x8044 sets full screen, the same way). The Window thread's loop
// (0x1422BC210) empties its queue, calls Sleep(1) and goes round again, and a
// sent message is only taken by PeekMessage: every frame's Present waits for
// that Sleep to end, up to a millisecond and a bit.
//
// The game's import of Sleep (a pointer in its
// import table, as main.cpp does for ExitProcess) goes to a stub. On the
// Window thread a wait of 1 ms becomes a wait of at most 1 ms that ends as
// soon as a message is sent to that thread (MsgWaitForMultipleObjectsEx with
// QS_SENDMESSAGE): the loop then takes Present at once. Posted messages and
// input wake it no sooner than before. Every other thread, and every other
// wait, calls Sleep as before (and the Window thread too, should the wait
// fail). Every 30 s the stats are logged (xerces.present_wake.stats): how
// many waits there were, how long those a sent message ended had lasted, and
// how long the others did (what a Sleep(1) lasts with this PC's timer).
//
// The same is done for the Input thread (0x1422BB4A0), which
// takes the raw mouse input with GetRawInputBuffer and then calls Sleep(1):
// its wait ends as soon as raw input comes (QS_RAWINPUT), so a mouse
// movement counts at once instead of after up to a millisecond.
//
// Earlier detours of kernel32's Sleep code crashed the game in loads
// (docs/research/upstream-render.md); this changes only the game's own
// import pointer, takes no lock and asks for no memory.
//
// Both are on unless -xerces-no-present-wake or -xerces-no-input-wake is given
// (xerces_core/default_features.hpp; opt-in with -xerces-present-wake and
// -xerces-input-wake until 2026-11-14, after map changes, mod stacks and the
// soak had been played with them; those flags change nothing now).

#include <component/scheduler.hpp>
#include <game/game.hpp>
#include <loader/component_loader.hpp>

#include <utils/hook.hpp>
#include <utils/nt.hpp>

#include <xerces_core/default_features.hpp>
#include <xerces_core/launch.hpp>

#include "runtime.hpp"

namespace xerces_present_wake {
namespace {
// The Window and Input contexts in the engine's thread table
// (xerces_core::engine_thread_name)
constexpr std::size_t window_context = 21;
constexpr std::size_t input_context = 22;

using sleep_fn = void(WINAPI *)(DWORD);
sleep_fn game_sleep = nullptr;
std::int64_t qpc_frequency = 1;
bool present_on = false;
bool input_on = false;

struct wake_stats {
  std::atomic<std::uint64_t> waits{0};
  // Waits what they wait for ended, and how long they had lasted
  std::atomic<std::uint64_t> early{0};
  std::atomic<std::uint64_t> early_us{0};
  // Waits that ran their time, and how long that was: what Sleep(1) lasts
  // with the timer this PC has
  std::atomic<std::uint64_t> full{0};
  std::atomic<std::uint64_t> full_us{0};
};
wake_stats present_stats;
wake_stats input_stats;

// A context's thread ID while it runs: the engine's own table (its ID, and
// its function, which the game clears when the thread returns). Plain reads:
// both tables are in the game's image.
DWORD context_thread(const std::size_t context) {
  const std::uint64_t *ids = game::sys::s_threadContextIds.get();
  const std::uint64_t *functions = game::sys::s_threadContextFunctions.get();
  if (!ids || !functions) {
    return 0;
  }
  const volatile std::uint64_t *function = functions + context;
  const volatile std::uint64_t *id = ids + context;
  return *function != 0 ? static_cast<DWORD>(*id) : 0;
}

// Waits up to `ms` or until something of `mask` comes to this thread's
// queue; false when the wait failed (the caller then sleeps as before).
// No MWMO_INPUTAVAILABLE: what came before this wait and was not taken must
// not end every later wait at once (a Sleep(1) of the thread outside its
// loop, with no look at the queue between, would spin); only what comes
// during the wait ends it.
bool wait_waking(const DWORD ms, const DWORD mask, wake_stats &stats) {
  LARGE_INTEGER begin{};
  QueryPerformanceCounter(&begin);
  const DWORD woke = MsgWaitForMultipleObjectsEx(0, nullptr, ms, mask, 0);
  if (woke == WAIT_FAILED) {
    return false;
  }
  LARGE_INTEGER end{};
  QueryPerformanceCounter(&end);
  const auto us = static_cast<std::uint64_t>((end.QuadPart - begin.QuadPart) *
                                             1'000'000 / qpc_frequency);
  stats.waits.fetch_add(1, std::memory_order_relaxed);
  if (woke == WAIT_OBJECT_0) {
    stats.early.fetch_add(1, std::memory_order_relaxed);
    stats.early_us.fetch_add(us, std::memory_order_relaxed);
  } else {
    stats.full.fetch_add(1, std::memory_order_relaxed);
    stats.full_us.fetch_add(us, std::memory_order_relaxed);
  }
  return true;
}

// The two loops' calls of Sleep(1), at base 0x140000000 (the instruction
// after each call): only these waits end early. A Sleep(1) of the same
// thread anywhere else (a loop that does not look at its queue) would
// otherwise return at once while a message waits, and spin (a review,
// 2026-11-14).
constexpr std::uintptr_t window_loop_sleep = 0x1422BC302;
constexpr std::uintptr_t input_loop_sleep = 0x1422BB5D6;

bool called_from(const void *return_address, const std::uintptr_t site) {
  return reinterpret_cast<std::uintptr_t>(return_address) ==
         game::relocate(site);
}

void WINAPI sleep_stub(const DWORD ms) {
  if (ms == 1) {
    const void *caller = _ReturnAddress();
    const DWORD self = GetCurrentThreadId();
    if (present_on && called_from(caller, window_loop_sleep) &&
        self == context_thread(window_context)) {
      if (wait_waking(ms, QS_SENDMESSAGE, present_stats)) {
        return;
      }
    } else if (input_on && called_from(caller, input_loop_sleep) &&
               self == context_thread(input_context)) {
      // The Input thread takes its raw input with GetRawInputBuffer, which
      // is no look at the queue: GetQueueStatus marks what is there as
      // seen, so only input that comes during the wait ends it
      GetQueueStatus(QS_RAWINPUT);
      if (wait_waking(ms, QS_RAWINPUT, input_stats)) {
        return;
      }
    }
  }
  game_sleep(ms);
}

void report_one(wake_stats &stats, const std::string_view event,
                const std::string_view message, const std::size_t context) {
  // Read one at a time: a wait counted at the edge may land in the next 30 s
  const std::uint64_t w = stats.waits.exchange(0);
  const std::uint64_t e = stats.early.exchange(0);
  const std::uint64_t e_us = stats.early_us.exchange(0);
  const std::uint64_t f = stats.full.exchange(0);
  const std::uint64_t f_us = stats.full_us.exchange(0);
  // A wait that was ended lasted mean_us_woken; without the stub it would
  // have lasted about mean_us_full (a whole Sleep(1) here)
  xerces_runtime::log_info(
      event, message,
      std::format("{{\"waits\":{},\"woken\":{},\"mean_us_woken\":{},"
                  "\"full_waits\":{},\"mean_us_full\":{},\"thread\":{}}}",
                  w, e, e ? e_us / e : 0, f, f ? f_us / f : 0,
                  context_thread(context)));
}

void report() {
  if (present_on) {
    report_one(present_stats, "xerces.present_wake.stats",
               "Present without the message loop's millisecond, the last 30 s",
               window_context);
  }
  if (input_on) {
    report_one(input_stats, "xerces.input_wake.stats",
               "Raw mouse input without the input loop's millisecond, the "
               "last 30 s",
               input_context);
  }
}

} // namespace

class component final : public client_component {
  DEFINE_COMPONENT_NAME("xerces_present_wake");

public:
  void post_unpack() override {
    // The thread table's address is the current client's
    if (!game::is_new_client()) {
      return;
    }
    namespace features = xerces_core::default_features;
    const auto args = xerces_runtime::command_line_args();
    present_on = features::on(args, features::no_present_wake);
    input_on = features::on(args, features::no_input_wake);
    if (!present_on) {
      xerces_runtime::log_info("xerces.present_wake.opted_out",
                               "Present waits as the game always did "
                               "(-xerces-no-present-wake)");
    }
    if (!input_on) {
      xerces_runtime::log_info("xerces.input_wake.opted_out",
                               "Raw mouse input waits as the game always did "
                               "(-xerces-no-input-wake)");
    }
    if (!present_on && !input_on) {
      return;
    }
    void *entry = utils::nt::library{}.get_iat_entry("kernel32.dll", "Sleep");
    if (!entry) {
      if (present_on) {
        xerces_runtime::log_warn("xerces.present_wake.off",
                                 "The game's import of Sleep was not found");
      }
      if (input_on) {
        xerces_runtime::log_warn("xerces.input_wake.off",
                                 "The game's import of Sleep was not found");
      }
      return;
    }
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    qpc_frequency = frequency.QuadPart > 0 ? frequency.QuadPart : 1;
    game_sleep = *static_cast<sleep_fn *>(entry);
    utils::hook::set(entry, &sleep_stub);
    if (present_on) {
      xerces_runtime::log_info(
          "xerces.present_wake.on",
          "The game's Window thread wakes for Present as soon as it is sent "
          "(-xerces-no-present-wake turns it off)");
    }
    if (input_on) {
      xerces_runtime::log_info(
          "xerces.input_wake.on",
          "The game's Input thread wakes for raw mouse input as soon as it "
          "comes (-xerces-no-input-wake turns it off)");
    }
    scheduler::loop(report, scheduler::async, 30s);
  }
};
} // namespace xerces_present_wake

REGISTER_COMPONENT(xerces_present_wake::component)
