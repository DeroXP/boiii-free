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
// With -xerces-present-wake the game's import of Sleep (a pointer in its
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
// Earlier detours of kernel32's Sleep code crashed the game in loads
// (docs/research/upstream-render.md); this changes only the game's own
// import pointer, takes no lock and asks for no memory. Opt-in until it has
// been played through map changes.

#include <component/scheduler.hpp>
#include <game/game.hpp>
#include <loader/component_loader.hpp>

#include <utils/hook.hpp>
#include <utils/nt.hpp>

#include <xerces_core/launch.hpp>

#include "runtime.hpp"

namespace xerces_present_wake {
namespace {
constexpr std::string_view flag = "-xerces-present-wake";
// The Window context in the engine's thread table (xerces_core::
// engine_thread_name)
constexpr std::size_t window_context = 21;

using sleep_fn = void(WINAPI *)(DWORD);
sleep_fn game_sleep = nullptr;

std::atomic<std::uint64_t> waits{0};
// Waits a sent message ended, and how long they had lasted
std::atomic<std::uint64_t> early{0};
std::atomic<std::uint64_t> early_us{0};
// Waits that ran their time, and how long that was: what Sleep(1) lasts
// with the timer this PC has
std::atomic<std::uint64_t> full{0};
std::atomic<std::uint64_t> full_us{0};
std::int64_t qpc_frequency = 1;

// The Window thread's ID while it runs: the engine's own table (its ID, and
// its function, which the game clears when the thread returns). Plain reads:
// both tables are in the game's image.
DWORD window_thread() {
  const std::uint64_t *ids = game::sys::s_threadContextIds.get();
  const std::uint64_t *functions = game::sys::s_threadContextFunctions.get();
  if (!ids || !functions) {
    return 0;
  }
  const volatile std::uint64_t *function = functions + window_context;
  const volatile std::uint64_t *id = ids + window_context;
  return *function != 0 ? static_cast<DWORD>(*id) : 0;
}

void WINAPI sleep_stub(const DWORD ms) {
  if (ms != 1 || GetCurrentThreadId() != window_thread()) {
    game_sleep(ms);
    return;
  }
  LARGE_INTEGER begin{};
  QueryPerformanceCounter(&begin);
  // No MWMO_INPUTAVAILABLE: a message sent before this wait and not yet
  // taken must not end every later wait at once (a Sleep(1) of this thread
  // outside the loop, with no PeekMessage between, would spin); only a
  // message sent during the wait ends it
  const DWORD woke =
      MsgWaitForMultipleObjectsEx(0, nullptr, 1, QS_SENDMESSAGE, 0);
  if (woke == WAIT_FAILED) {
    game_sleep(ms);
    return;
  }
  LARGE_INTEGER end{};
  QueryPerformanceCounter(&end);
  const auto us = static_cast<std::uint64_t>((end.QuadPart - begin.QuadPart) *
                                             1'000'000 / qpc_frequency);
  waits.fetch_add(1, std::memory_order_relaxed);
  if (woke == WAIT_OBJECT_0) {
    early.fetch_add(1, std::memory_order_relaxed);
    early_us.fetch_add(us, std::memory_order_relaxed);
  } else {
    full.fetch_add(1, std::memory_order_relaxed);
    full_us.fetch_add(us, std::memory_order_relaxed);
  }
}

void report() {
  // Read one at a time: a wait counted at the edge may land in the next 30 s
  const std::uint64_t w = waits.exchange(0);
  const std::uint64_t e = early.exchange(0);
  const std::uint64_t e_us = early_us.exchange(0);
  const std::uint64_t f = full.exchange(0);
  const std::uint64_t f_us = full_us.exchange(0);
  // A wait a sent message ended lasted mean_us_woken; without the stub it
  // would have lasted about mean_us_full (a whole Sleep(1) here)
  xerces_runtime::log_info(
      "xerces.present_wake.stats",
      "Present without the message loop's millisecond, the last 30 s",
      std::format(
          "{{\"waits\":{},\"woken_by_message\":{},\"mean_us_woken\":{},"
          "\"full_waits\":{},\"mean_us_full\":{},\"window_thread\":{}}}",
          w, e, e ? e_us / e : 0, f, f ? f_us / f : 0, window_thread()));
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
    const auto args = xerces_runtime::command_line_args();
    if (!xerces_core::has_launch_flag(args, flag) &&
        !xerces_core::has_launch_flag(args, "-" + std::string(flag))) {
      return;
    }
    void *entry = utils::nt::library{}.get_iat_entry("kernel32.dll", "Sleep");
    if (!entry) {
      xerces_runtime::log_warn("xerces.present_wake.off",
                               "The game's import of Sleep was not found");
      return;
    }
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    qpc_frequency = frequency.QuadPart > 0 ? frequency.QuadPart : 1;
    game_sleep = *static_cast<sleep_fn *>(entry);
    utils::hook::set(entry, &sleep_stub);
    xerces_runtime::log_info(
        "xerces.present_wake.on",
        "The game's Window thread wakes for Present as soon as it is sent "
        "(-xerces-present-wake)");
    scheduler::loop(report, scheduler::async, 30s);
  }
};
} // namespace xerces_present_wake

REGISTER_COMPONENT(xerces_present_wake::component)
