#pragma once

// The precise frame limiter (docs/DESIGN.md 12.4 item 4). On only when the
// game starts with -xerces-precise-limiter (the launcher's "Precise FPS limit
// (experimental)"), so it can be A/B tested. The client's part is
// src/client/xerces/frame_pacing.cpp; the schedule and the waits are here, so
// a fake clock can test them with the engine's own check.
//
// What the engine does, read on the 2026-11-04 dump of build 0x6531394
// (Com_Frame_Try_Block_Function, 0x1420EC680 in upstream's table):
// - It reads com_maxfps (registered with hash 0x29B732E8, default 60, 0 to
//   1000) and takes the smaller of it and Com_FPSLimit, which upstream makes
//   return UINT32_MAX, so com_maxfps alone counts.
// - The target is 1000 / com_maxfps whole milliseconds, at least 1; 1 also
//   for com_maxfps 0.
// - Then, up to 50 times: run the event loop, read Sys_Milliseconds
//   (timeGetTime() minus its first value), set last to now if the clock went
//   back, and stop when now - last >= target. Otherwise call the 1 ms wait
//   (the call at +0x113, upstream's frame_limiter_sleep) and go round again.
//   After 50 waits the frame goes anyway, so a target over 50 ms (com_maxfps
//   1 to 19) holds a frame for at most about 51 to 57 ms.
// - On the way out, last becomes the final reading, and now - last is the
//   frame's time in whole milliseconds.
// timeGetTime only moves when the system timer ticks. At com_maxfps 240 the
// target is 4 ms, and a 1 ms wait that overshoots lets a frame go anywhere
// from about 3 to 5 ms after the last one.
//
// What the pacer does instead, for com_maxfps 20 and up:
// - At each frame's start (scheduler.cpp's detour of that function, before
//   the engine's code) it waits until one period of com_maxfps, 1000 /
//   com_maxfps ms (4.167 ms at 240), after it last let a frame go: on a
//   high-resolution waitable timer until 0.3 ms before, then spinning on QPC.
//   A period is at most 50 ms, so the event loop runs at least that often.
// - The engine's check then normally passes at once. Two of its readings are
//   one period apart, plus the change in the event loop's time, and a period
//   is never less than the target. When the system timer ticks at a fixed
//   interval that divides 1 ms (1 or 0.5 ms), readings at least `target` ms
//   apart always count `target` steps.
// - When the check still fails (a period of whole milliseconds, such as 5 ms
//   at 200, meets jitter, or the clock lags), the engine calls its wait. The
//   pacer spins until the engine's clock moves, the step the check is missing
//   (at most 1 ms, once per frame). After that it is upstream's 1 ms wait.
// - Frames slower than the period are not held. With com_maxfps 0, and under
//   20 where the engine can give up before the cap (above), nothing changes:
//   the engine's wait is upstream's.
// - The function also times itself with QPC from its own start, into a
//   32-frame average that only a performance record reads (frameavg). The
//   frame-start wait is outside that timing.
// The pacer lets a frame go when its wait ends; the engine then still runs
// its event loop before it reads the clock, so the frame really starts that
// much later. That time is the engine's and the pacer cannot see it.
// Only the start of a frame and the engine's wait call into the pacer, both
// on the main thread. The waits are calls to the timer and to QPC, never
// detours of Sleep or WaitFor* (DESIGN.md 12.4 item 6).

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace xerces_core::frame_pacing {
// The launch flag, and its name as the launcher's settings handle flags
// (settings_options' has_flag and with_flag take names without the dash).
inline constexpr std::string_view flag = "-xerces-precise-limiter";
inline constexpr std::string_view flag_name = "xerces-precise-limiter";

// The flag before the first "+" command, spelled "-name" or "--name".
bool requested(const std::vector<std::string> &args);

// ------------------------------------------------ the engine's own limiter
inline constexpr int engine_max_passes = 50;

// The engine's target in whole milliseconds for a com_maxfps value.
int engine_target_ms(int maxfps);

// One pass of the engine's check on its millisecond clock. Like the engine,
// moves `last_ms` back to `now_ms` when the clock went back (a signed
// comparison), then returns whether now_ms - last_ms >= target_ms.
bool engine_pass(std::int32_t now_ms, std::int32_t &last_ms, int target_ms);

// Whether the pacer paces a com_maxfps value: 20 and up. Each of the
// engine's 50 waits lasts at least 1 ms, so it can hold a frame for its
// whole target only when that is 50 ms or less. Under 20 it can give up
// first, and the pacer leaves those caps as they were, like 0.
bool paces(int maxfps);

// ------------------------------------------------------------ the schedule
struct timing {
  std::int64_t freq = 0;        // QPC ticks per second
  std::int64_t spin = 0;        // the end of a wait, spun on QPC: 0.3 ms
  std::int64_t tick_spin = 0;   // the longest spin for the engine's clock: 1 ms
  std::int64_t release_bin = 0; // one bin of the release counts: 10 us
};
timing timing_for(std::int64_t freq);

// One period of com_maxfps in QPC ticks, rounded to the nearest tick; 0 for
// a cap the pacer leaves alone (paces() is false).
std::int64_t period_ticks(int maxfps, std::int64_t freq);

// When the next frame may start: one period after the last one was let go.
// 0 means no wait: a cap the pacer leaves alone, or nothing let go yet.
std::int64_t next_deadline(std::int64_t released, int maxfps, const timing &t);

// The wait towards `deadline` from `now`: on the timer until `sleep_until`,
// then spin until `until`, the deadline. `until <= now` means no wait, and
// `sleep_until <= now` means spin only.
struct wait_step {
  std::int64_t sleep_until = 0;
  std::int64_t until = 0;
};
wait_step step_towards(std::int64_t deadline, std::int64_t now,
                       const timing &t);

// How long after its deadline the pacer let each frame that waited go, once
// the engine's check had passed: 10 us bins from 0 to 2 ms, the last bin
// also counting everything later.
inline constexpr int release_bins = 200;
inline constexpr int release_bin_us = 10;

// What the pacer did, for the game log. Main thread only; the client copies
// it out from there.
struct counters {
  std::uint64_t frames = 0;      // frame starts with a cap the pacer paces
  std::uint64_t paced = 0;       // ... that waited
  std::uint64_t tick_waits = 0;  // the engine's check failed after the deadline
  std::uint64_t tick_missed = 0; // ... and its clock did not move within 1 ms
  std::uint64_t upstream_waits = 0; // upstream's 1 ms wait
  std::uint64_t timer_errors = 0;   // the timer could not wait: spun instead
  std::int64_t spin = 0;            // ticks spent spinning
  std::array<std::uint32_t, release_bins> released{}; // see release_bins
  std::int64_t released_max = 0; // ticks: the latest release after a deadline
};

// Frames in the release counts.
std::uint64_t released_count(const counters &c);
// ... let go less than `us` after their deadline (`us` a multiple of 10).
std::uint64_t released_under(const counters &c, int us);
// The release delay `share` of them stay at or under, in us: the upper edge
// of its bin, or the latest one when that is past the last bin. 0 for none.
std::int64_t released_percentile_us(const counters &c, double share,
                                    std::int64_t freq);

// {"frames":..,"paced":..,"released_under_100us":..,"released_p99_us":..,
// "released_max_us":..,"tick_waits":..,"tick_missed":..,"upstream_waits":..,
// "timer_errors":..,"spin_us":..,"maxfps":..}
std::string counters_json(const counters &c, std::int64_t freq, int maxfps);

// The pacer. Platform provides, on the thread that runs the frames:
//   std::int64_t now();           QPC
//   std::uint32_t engine_ms();    the engine's clock source, timeGetTime()
//   bool sleep(std::int64_t ticks);  the high-resolution timer; false when
//                                 it could not wait (it returns at once and
//                                 the pacer spins instead)
//   void pause();                 one spin step (YieldProcessor)
//   bool upstream_wait(std::uint32_t ms);  upstream's frame_limiter_sleep;
//                                 false when the timer failed and it spun
//   int maxfps();                 com_maxfps, 0 when it cannot be read
template <class Platform> class pacer {
public:
  explicit pacer(const timing &t) : t_(t) {}

  // Before the engine's frame (scheduler.cpp's main_frame_stub).
  void frame_start(Platform &p) {
    settle_release();
    maxfps_ = p.maxfps();
    paces_ = paces(maxfps_);
    tick_spun_ = false;
    const std::int64_t now = p.now();
    deadline_ = next_deadline(released_, maxfps_, t_);
    held_ = paces_ && deadline_ > now;
    if (paces_) {
      ++c_.frames;
      if (held_) {
        ++c_.paced;
      }
    }
    released_ = wait(p, now);
  }

  // The engine's check failed (upstream's frame_limiter_sleep, `ms` is 1).
  // The frame-start wait already reached the deadline, so what is missing is
  // a step of the engine's clock.
  void limiter_wait(Platform &p, std::uint32_t ms) {
    if (paces_ && !tick_spun_) {
      released_ = spin_for_tick(p, p.now());
      return;
    }
    ++c_.upstream_waits;
    if (!p.upstream_wait(ms)) {
      ++c_.timer_errors;
    }
    released_ = p.now();
  }

  const counters &stats() const { return c_; }
  // The counters since the last call, for one log line.
  counters take_stats() {
    const counters out = c_;
    c_ = {};
    return out;
  }

private:
  // Returns when the wait ended.
  std::int64_t wait(Platform &p, std::int64_t now) {
    const wait_step step = step_towards(deadline_, now, t_);
    if (step.until <= now) {
      return now;
    }
    std::int64_t t = now;
    if (step.sleep_until > t) {
      if (!p.sleep(step.sleep_until - t)) {
        ++c_.timer_errors;
      }
      t = p.now();
    }
    const std::int64_t spun_from = t;
    while (t < step.until) {
      p.pause();
      t = p.now();
    }
    c_.spin += t - spun_from;
    return t;
  }

  std::int64_t spin_for_tick(Platform &p, std::int64_t now) {
    tick_spun_ = true;
    ++c_.tick_waits;
    const std::uint32_t start = p.engine_ms();
    std::int64_t t = now;
    bool moved = false;
    while (t - now < t_.tick_spin) {
      p.pause();
      t = p.now();
      if (p.engine_ms() != start) {
        moved = true;
        break;
      }
    }
    if (!moved) {
      ++c_.tick_missed;
    }
    c_.spin += t - now;
    return t;
  }

  // The frame before this one is final now: count when it was let go,
  // after its frame-start wait and any spin or wait the engine's check
  // asked for.
  void settle_release() {
    if (!held_) {
      return;
    }
    held_ = false;
    const std::int64_t after = std::max<std::int64_t>(released_ - deadline_, 0);
    const std::int64_t bin =
        std::min<std::int64_t>(after / t_.release_bin, release_bins - 1);
    ++c_.released[static_cast<std::size_t>(bin)];
    c_.released_max = std::max(c_.released_max, after);
  }

  timing t_;
  std::int64_t released_ = 0; // QPC when the last frame was let go
  std::int64_t deadline_ = 0; // this frame's deadline, 0 for none
  int maxfps_ = 0;            // com_maxfps at this frame's start
  bool paces_ = false;        // ... and whether the pacer paces it
  bool held_ = false;         // this frame waited for its deadline
  bool tick_spun_ = false;    // this frame already spun for the engine's clock
  counters c_;
};
} // namespace xerces_core::frame_pacing
