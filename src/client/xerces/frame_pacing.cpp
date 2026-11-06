#include <std_include.hpp>

// The precise frame limiter (frame_pacing.hpp, xerces_core/frame_pacing.hpp,
// docs/DESIGN.md 12.4 item 4): the platform the pacer runs on in the game.
// QPC is the clock, a high-resolution waitable timer does most of each wait,
// timeGetTime is the source of the engine's own millisecond clock, and
// com_maxfps is read through the dvar the engine reads. No engine address is
// added: the pacer runs inside two hooks that already exist.
//
// Every 30 s the main pipeline takes the counters and the async pipeline
// logs them (xerces.frame_pacing.stats), so a test run shows how close to
// their deadlines frames were let go, how often the engine's check still
// missed a step and how long the pacer spun. The line comes every 30 s even
// when nothing was paced (com_maxfps 0), so a tester can see the pacer is on
// and which com_maxfps it read.

#include <timeapi.h>

#include <component/scheduler.hpp>
#include <game/game.hpp>
#include <game/utils.hpp>
#include <loader/component_loader.hpp>

#include <xerces_core/frame_pacing.hpp>

#include "frame_pacing.hpp"
#include "runtime.hpp"

namespace xerces_frame_pacing {
namespace detail {
std::atomic_bool armed{false};
} // namespace detail

namespace {
namespace core = xerces_core::frame_pacing;

class platform {
public:
  bool open() {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    freq_ = f.QuadPart;
    timer_ = CreateWaitableTimerExW(nullptr, nullptr,
                                    CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                    TIMER_ALL_ACCESS);
    return timer_ != nullptr && freq_ > 0;
  }

  std::int64_t freq() const { return freq_; }

  std::int64_t now() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
  }

  // Sys_Milliseconds is timeGetTime() minus its first value.
  std::uint32_t engine_ms() { return timeGetTime(); }

  // False when the timer could not wait: the pacer then spins instead.
  bool sleep(const std::int64_t ticks) {
    return wait_100ns(ticks * 10'000'000 / freq_);
  }

  void pause() { YieldProcessor(); }

  // What upstream's frame_limiter_sleep does: the same wait on a
  // high-resolution timer. When the timer fails, spins for the rest.
  bool upstream_wait(const std::uint32_t ms) {
    const std::int64_t until =
        now() + static_cast<std::int64_t>(ms) * freq_ / 1'000;
    if (wait_100ns(static_cast<std::int64_t>(ms) * 10'000)) {
      return true;
    }
    while (now() < until) {
      YieldProcessor();
    }
    return false;
  }

  int maxfps() {
    if (!com_maxfps_) {
      com_maxfps_ = game::get_dvar("com_maxfps");
    }
    return com_maxfps_ ? com_maxfps_.get_int() : 0;
  }

private:
  // A timer that was not set never signals, so the result of setting it is
  // checked, and the wait has a limit: twice the time asked for plus 5 ms. A
  // wait past that cancels the timer and takes a signal that came in
  // between, so the next wait does not end early.
  bool wait_100ns(const std::int64_t units) {
    const std::int64_t wait = std::max<std::int64_t>(units, 1);
    LARGE_INTEGER due{};
    due.QuadPart = -wait;
    if (!SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
      return false;
    }
    const auto limit_ms = static_cast<DWORD>(wait * 2 / 10'000 + 5);
    if (WaitForSingleObject(timer_, limit_ms) != WAIT_OBJECT_0) {
      CancelWaitableTimer(timer_);
      WaitForSingleObject(timer_, 0);
      return false;
    }
    return true;
  }

  HANDLE timer_ = nullptr;
  std::int64_t freq_ = 0;
  game::EngineDependentDvarMut com_maxfps_{};
};

platform the_platform;
std::optional<core::pacer<platform>> the_pacer;

void report() {
  const core::counters c = the_pacer->take_stats();
  std::string data =
      core::counters_json(c, the_platform.freq(), the_platform.maxfps());
  scheduler::once(
      [data = std::move(data)] {
        xerces_runtime::log_info("xerces.frame_pacing.stats",
                                 "Precise frame limiter, the last 30 s", data);
      },
      scheduler::async);
}
} // namespace

void detail::frame_start() { the_pacer->frame_start(the_platform); }

void detail::limiter_wait(const std::uint32_t ms) {
  the_pacer->limiter_wait(the_platform, ms);
}

class component final : public client_component {
  DEFINE_COMPONENT_NAME("xerces_frame_pacing");

public:
  void post_unpack() override {
    // Upstream's wait is in the new client only (client_patches.cpp).
    if (!game::is_new_client()) {
      return;
    }
    if (!core::requested(xerces_runtime::command_line_args())) {
      return;
    }
    if (!the_platform.open()) {
      xerces_runtime::log_warn(
          "xerces.frame_pacing.off",
          "Precise frame limiter off: no high-resolution waitable timer "
          "(Windows 10 1803 or later)",
          "{\"error\":" + std::to_string(GetLastError()) + "}");
      return;
    }
    the_pacer.emplace(core::timing_for(the_platform.freq()));
    detail::armed.store(true, std::memory_order_release);
    xerces_runtime::log_info(
        "xerces.frame_pacing.on",
        "Precise frame limiter on: frames start one com_maxfps period apart "
        "(com_maxfps 20 and up)",
        "{\"qpc_freq\":" + std::to_string(the_platform.freq()) + "}");
    scheduler::loop(report, scheduler::main, 30s);
  }
};
} // namespace xerces_frame_pacing

REGISTER_COMPONENT(xerces_frame_pacing::component)
