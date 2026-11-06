// The precise frame limiter (docs/DESIGN.md 12.4 item 4) on a fake clock:
// the engine's own whole-millisecond limiter as the dump shows it, with
// upstream's 1 ms wait for comparison, then with the pacer.

#include "harness.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include <xerces_core/frame_pacing.hpp>
#include <xerces_core/launch.hpp>
#include <xerces_core/settings_options.hpp>

using namespace xerces_core;
namespace fp = xerces_core::frame_pacing;

namespace {
constexpr std::int64_t freq = 10'000'000; // QPC on current Windows
constexpr std::int64_t us = freq / 1'000'000;
constexpr std::int64_t ms = freq / 1'000;

// Deterministic jitter, uniform in [lo, hi] ticks.
class jitter {
public:
  explicit jitter(std::uint64_t seed) : rng_(seed) {}
  std::int64_t between(std::int64_t lo, std::int64_t hi) {
    if (hi <= lo) {
      return lo;
    }
    return lo + static_cast<std::int64_t>(
                    rng_() % static_cast<std::uint64_t>(hi - lo + 1));
  }

private:
  std::mt19937_64 rng_;
};

// The PC as the pacer sees it: QPC, timeGetTime moving only at the system
// timer's interrupts, and a high-resolution timer that wakes a little late.
struct fake_platform {
  std::int64_t t = 1'000 * ms;
  std::int64_t interrupt = ms; // the system timer's interval
  std::int64_t phase = 0;      // where its interrupts fall
  std::int64_t timer_late_lo = 20 * us;
  std::int64_t timer_late_hi = 150 * us;
  std::int64_t extra_late = 0; // added once to the next timer wake
  std::int64_t pause_step = 2; // 0.2 us per spin step
  int fps = 240;
  jitter rng{11};
  std::int64_t longest_sleep = 0;

  std::int64_t now() { return t; }
  std::uint32_t engine_ms() {
    const std::int64_t last_interrupt =
        phase + ((t - phase) / interrupt) * interrupt;
    return static_cast<std::uint32_t>(last_interrupt / ms);
  }
  void sleep(std::int64_t ticks) {
    longest_sleep = std::max(longest_sleep, ticks);
    t += ticks + rng.between(timer_late_lo, timer_late_hi) + extra_late;
    extra_late = 0;
  }
  void pause() { t += pause_step; }
  void upstream_wait(std::uint32_t wait_ms) {
    t += wait_ms * ms + rng.between(timer_late_lo, timer_late_hi);
  }
  int maxfps() { return fps; }
};

using pacer = fp::pacer<fake_platform>;

struct run_result {
  std::vector<std::int64_t> releases; // QPC when each frame was let go
  std::vector<int> waits;             // the engine's wait calls per frame
  std::vector<std::int32_t> msec;     // the frame time the engine measured
  fp::counters stats;

  // Milliseconds between frames let go, from frame `from` on.
  std::vector<double> intervals(std::size_t from = 5) const {
    std::vector<double> out;
    for (std::size_t i = std::max<std::size_t>(from, 1); i < releases.size();
         ++i) {
      out.push_back(static_cast<double>(releases[i] - releases[i - 1]) / ms);
    }
    return out;
  }
};

double lowest(const std::vector<double> &v) {
  return *std::min_element(v.begin(), v.end());
}
double highest(const std::vector<double> &v) {
  return *std::max_element(v.begin(), v.end());
}
double mean(const std::vector<double> &v) {
  return std::accumulate(v.begin(), v.end(), 0.0) /
         static_cast<double>(v.size());
}

// Runs `frames` calls of Com_Frame_Try_Block_Function: the pacer's start
// (when paced), the engine's limiter loop exactly as the dump has it, then the
// frame's own work. `work(i, p)` returns frame i's work and may change the
// platform first (com_maxfps, a late timer).
run_result run(fake_platform &p, bool paced, int frames,
               const std::function<std::int64_t(int, fake_platform &)> &work,
               std::int64_t pump_lo = 5 * us, std::int64_t pump_hi = 40 * us) {
  run_result out;
  pacer pc(fp::timing_for(freq));
  jitter engine_rng(29);
  // As if the frame before the first one ended long ago.
  std::int32_t last = static_cast<std::int32_t>(p.engine_ms()) - 1000;
  for (int i = 0; i < frames; ++i) {
    const std::int64_t own_work = work(i, p);
    if (paced) {
      pc.frame_start(p);
    }
    const int target = fp::engine_target_ms(p.fps);
    std::int32_t now = 0;
    int waits = 0;
    for (;;) {
      p.t += engine_rng.between(pump_lo, pump_hi); // the event loop
      now = static_cast<std::int32_t>(p.engine_ms());
      if (fp::engine_pass(now, last, target)) {
        break;
      }
      if (paced) {
        pc.limiter_wait(p, 1);
      } else {
        p.upstream_wait(1);
      }
      if (++waits >= fp::engine_max_passes) {
        break;
      }
    }
    out.msec.push_back(now - last);
    last = now;
    out.releases.push_back(p.t);
    out.waits.push_back(waits);
    p.t += own_work;
  }
  out.stats = pc.stats();
  return out;
}

std::function<std::int64_t(int, fake_platform &)>
uniform_work(std::int64_t lo, std::int64_t hi, std::uint64_t seed = 7) {
  auto rng = std::make_shared<jitter>(seed);
  return [rng, lo, hi](int, fake_platform &) { return rng->between(lo, hi); };
}

constexpr double period_240 = 1000.0 / 240.0;
} // namespace

TEST_CASE("frame pacing: the engine's target is whole milliseconds") {
  CHECK_EQ(fp::engine_target_ms(0), 1); // no cap still waits for one step
  CHECK_EQ(fp::engine_target_ms(-3), 1);
  CHECK_EQ(fp::engine_target_ms(1), 1000);
  CHECK_EQ(fp::engine_target_ms(60), 16);
  CHECK_EQ(fp::engine_target_ms(144), 6);
  CHECK_EQ(fp::engine_target_ms(165), 6);
  CHECK_EQ(fp::engine_target_ms(240), 4);
  CHECK_EQ(fp::engine_target_ms(250), 4);
  CHECK_EQ(fp::engine_target_ms(333), 3);
  CHECK_EQ(fp::engine_target_ms(1000), 1);
  CHECK_EQ(fp::engine_target_ms(1001), 1);
}

TEST_CASE("frame pacing: the engine's check, and its clock going back") {
  std::int32_t last = 100;
  CHECK(!fp::engine_pass(103, last, 4));
  CHECK_EQ(last, 100);
  CHECK(fp::engine_pass(104, last, 4));
  CHECK_EQ(last, 100); // the engine sets last after its loop, not here
  CHECK(!fp::engine_pass(90, last, 1)); // went back: last follows, 0 elapsed
  CHECK_EQ(last, 90);
  // 32-bit arithmetic, as the engine's sub and cmp do it.
  last = std::numeric_limits<std::int32_t>::max() - 1;
  CHECK(fp::engine_pass(std::numeric_limits<std::int32_t>::min() + 2, last, 4));
  CHECK_EQ(last, std::numeric_limits<std::int32_t>::max() - 1);
}

TEST_CASE("frame pacing: the flag counts only before the first command") {
  CHECK(fp::requested({"-launch", "-xerces-precise-limiter", "+set", "a"}));
  CHECK(fp::requested({"--xerces-precise-limiter"}));
  CHECK(!fp::requested({"-launch", "+set", "name", "-xerces-precise-limiter"}));
  CHECK(!fp::requested({"-xerces-precise-limiters"}));
  CHECK(!fp::requested({}));

  // The launcher's setting adds it to the extra flags; the command keeps it
  // before the dvars, where the client looks.
  std::vector<std::string> flags =
      with_flag({"-windowed"}, fp::flag_name, true);
  CHECK((flags ==
         std::vector<std::string>{"-windowed", "-xerces-precise-limiter"}));
  CHECK(has_flag(flags, fp::flag_name));
  CHECK(parse_launch_flags("-xerces-precise-limiter").reserved.empty());
  launch_options options;
  options.extra_flags = flags;
  options.dvars = {{"com_maxfps", "240"}};
  const launch_command cmd = build_launch_command(nullptr, options);
  REQUIRE(cmd.ok());
  CHECK(fp::requested(cmd.args));
}

TEST_CASE("frame pacing: the schedule") {
  const fp::timing t = fp::timing_for(freq);
  CHECK_EQ(t.spin, 300 * us);
  CHECK_EQ(t.tick_spin, ms);
  CHECK_EQ(t.max_wait, 50 * ms);
  CHECK_EQ(t.late, 100 * us);

  CHECK_EQ(fp::period_ticks(240, freq), 41'667); // 4.1667 ms
  CHECK_EQ(fp::period_ticks(144, freq), 69'444);
  CHECK_EQ(fp::period_ticks(1000, freq), 10'000);
  CHECK_EQ(fp::period_ticks(0, freq), 0);
  CHECK_EQ(fp::period_ticks(-1, freq), 0);

  CHECK_EQ(fp::next_deadline(5'000'000, 240, t), 5'041'667);
  CHECK_EQ(fp::next_deadline(0, 240, t), 0); // nothing let go yet
  CHECK_EQ(fp::next_deadline(5'000'000, 0, t), 0);

  // The timer until 0.3 ms before the deadline, then a spin.
  fp::wait_step s = fp::step_towards(5'041'667, 5'010'000, t);
  CHECK_EQ(s.until, 5'041'667);
  CHECK_EQ(s.sleep_until, 5'038'667);
  // Under 0.3 ms left: spin only.
  s = fp::step_towards(5'041'667, 5'040'000, t);
  CHECK(s.sleep_until <= 5'040'000);
  CHECK_EQ(s.until, 5'041'667);
  // At or past the deadline, or none: no wait.
  CHECK(fp::step_towards(5'041'667, 5'041'667, t).until <= 5'041'667);
  CHECK(fp::step_towards(0, 5'041'667, t).until <= 5'041'667);
  // Far away: cut at 50 ms, without the spin.
  s = fp::step_towards(7'000'000, 5'000'000, t);
  CHECK_EQ(s.until, 5'500'000);
  CHECK_EQ(s.sleep_until, 5'500'000);
}

TEST_CASE("frame pacing: com_maxfps 240 lets frames go every 4.167 ms") {
  for (const std::int64_t interrupt : {ms, ms / 2}) {
    for (const std::int64_t phase : {std::int64_t{0}, 3 * ms / 10}) {
      fake_platform p;
      p.interrupt = interrupt;
      p.phase = phase % interrupt;
      const run_result r =
          run(p, true, 3000, uniform_work(1 * ms, 35 * ms / 10));
      const std::vector<double> gaps = r.intervals();
      // One period plus the event loop's jitter (35 us at most here).
      CHECK(lowest(gaps) >= period_240 - 0.04);
      CHECK(highest(gaps) <= period_240 + 0.04);
      CHECK(std::abs(mean(gaps) - period_240) < 0.001);
      // The engine's check passed on its first pass every time.
      CHECK(std::all_of(r.waits.begin() + 5, r.waits.end(),
                        [](int w) { return w == 0; }));
      CHECK_EQ(r.stats.tick_waits, 0u);
      CHECK_EQ(r.stats.upstream_waits, 0u);
      CHECK_EQ(r.stats.late, 0u);
      // The engine measures 4 or 5 whole ms, 4.167 on average.
      double sum = 0;
      for (std::size_t i = 5; i < r.msec.size(); ++i) {
        CHECK(r.msec[i] == 4 || r.msec[i] == 5);
        sum += r.msec[i];
      }
      CHECK(std::abs(sum / static_cast<double>(r.msec.size() - 5) -
                     period_240) < 0.01);

      // Upstream's 1 ms wait on the same frames: whole-millisecond steps.
      fake_platform q;
      q.interrupt = interrupt;
      q.phase = phase % interrupt;
      const run_result u =
          run(q, false, 3000, uniform_work(1 * ms, 35 * ms / 10));
      const std::vector<double> up = u.intervals();
      CHECK(highest(up) - lowest(up) > 0.9);
    }
  }
}

TEST_CASE("frame pacing: frames that already take most of the period") {
  // 3.6 to 4.1 ms of work at com_maxfps 240, with the system timer at 0.5 ms:
  // the engine's check often passes without calling its wait at all, so a
  // pacer inside that wait could not hold these frames to 4.167 ms.
  fake_platform q;
  q.interrupt = ms / 2;
  const run_result u =
      run(q, false, 2000, uniform_work(36 * ms / 10, 41 * ms / 10));
  const auto no_wait = std::count_if(u.waits.begin(), u.waits.end(),
                                     [](int w) { return w == 0; });
  CHECK(no_wait > 200);
  CHECK(lowest(u.intervals()) < 4.0);

  // Waiting at the frame's start holds every one of them.
  fake_platform p;
  p.interrupt = ms / 2;
  const run_result r =
      run(p, true, 2000, uniform_work(36 * ms / 10, 41 * ms / 10));
  CHECK(lowest(r.intervals()) >= period_240 - 0.04);
  CHECK(highest(r.intervals()) <= period_240 + 0.04);
}

TEST_CASE("frame pacing: frames slower than the period are not held") {
  fake_platform p;
  const run_result r = run(p, true, 1000, uniform_work(5 * ms, 7 * ms));
  fake_platform q;
  const run_result u = run(q, false, 1000, uniform_work(5 * ms, 7 * ms));
  CHECK_EQ(r.stats.paced, 0u);
  CHECK_EQ(r.stats.frames, 1000u);
  CHECK(r.releases == u.releases); // nothing added
  CHECK(lowest(r.intervals()) >= 5.0);
}

TEST_CASE("frame pacing: whole-millisecond periods still pass the check") {
  // At 200, 250 and 1000 FPS a period is exactly the engine's target, so the
  // check misses a step whenever the event loop ran shorter than the frame
  // before and a step falls in between. That step is then only the jitter
  // away, and the spin waits for exactly that. Once a frame has gone right
  // after a step, the following ones pass at once. Start times every 20 us
  // across a millisecond find the frames that miss.
  struct cap {
    int fps;
    std::int64_t work_lo, work_hi;
  };
  for (const cap c : {cap{200, 1 * ms, 4 * ms}, cap{250, 1 * ms, 3 * ms},
                      cap{1000, 2 * ms / 10, 6 * ms / 10}}) {
    for (const std::int64_t interrupt : {ms, ms / 2}) {
      std::uint64_t tick_waits = 0;
      for (std::int64_t start = 0; start < ms; start += 20 * us) {
        fake_platform p;
        p.fps = c.fps;
        p.interrupt = interrupt;
        p.t += start;
        const run_result r =
            run(p, true, 400, uniform_work(c.work_lo, c.work_hi));
        const double period = 1000.0 / c.fps;
        const std::vector<double> gaps = r.intervals();
        CHECK(lowest(gaps) >= period - 0.04);
        CHECK(highest(gaps) <= period + 0.05);
        CHECK(std::abs(mean(gaps) - period) < 0.01);
        CHECK_EQ(r.stats.tick_missed, 0u);
        CHECK_EQ(r.stats.upstream_waits, 0u);
        tick_waits += r.stats.tick_waits;
      }
      CHECK(tick_waits > 0u);
    }
  }
}

TEST_CASE("frame pacing: a system timer of 1/1024 s costs some late frames") {
  // Some PCs tick at 0.9766 ms, which does not divide 1 ms: timeGetTime then
  // skips a value now and then, and about one frame in ten waits for the
  // clock's next step. No frame is shorter than the period, none is longer
  // than the period plus one tick, and the spin always sees the step.
  fake_platform p;
  p.interrupt = 9'766;
  const run_result r = run(p, true, 3000, uniform_work(1 * ms, 35 * ms / 10));
  const std::vector<double> gaps = r.intervals();
  CHECK(lowest(gaps) >= period_240 - 0.04);
  CHECK(highest(gaps) <= period_240 + 0.9766 + 0.05);
  CHECK(mean(gaps) < period_240 + 0.1);
  CHECK(r.stats.tick_waits > 0u);
  CHECK(r.stats.tick_waits < 3000u / 5);
  CHECK_EQ(r.stats.tick_missed, 0u);
}

TEST_CASE("frame pacing: com_maxfps 0 keeps upstream's wait as it was") {
  // Menu frames of 0.1 to 0.5 ms: the engine still waits for one whole step.
  fake_platform p;
  p.fps = 0;
  const run_result r =
      run(p, true, 2000, uniform_work(1 * ms / 10, 5 * ms / 10));
  fake_platform q;
  q.fps = 0;
  const run_result u =
      run(q, false, 2000, uniform_work(1 * ms / 10, 5 * ms / 10));
  CHECK(r.releases == u.releases);
  CHECK(r.waits == u.waits);
  CHECK_EQ(r.stats.frames, 0u);
  CHECK(r.stats.upstream_waits > 1000u);
  CHECK_EQ(r.stats.spin, 0);
}

TEST_CASE("frame pacing: a com_maxfps change counts from the next frame") {
  auto base = uniform_work(1 * ms, 3 * ms);
  fake_platform p;
  const run_result r = run(p, true, 900, [base](int i, fake_platform &pl) {
    pl.fps = i < 300 ? 240 : i < 600 ? 60 : 144;
    return base(i, pl);
  });
  const std::vector<double> gaps = r.intervals(0); // gaps[i - 1]: frame i
  for (std::size_t i = 5; i < r.releases.size(); ++i) {
    const double period = i < 300   ? 1000.0 / 240
                          : i < 600 ? 1000.0 / 60
                                    : 1000.0 / 144;
    CHECK(std::abs(gaps[i - 1] - period) <= 0.04);
  }
}

TEST_CASE("frame pacing: one late timer wake makes one late frame") {
  auto base = uniform_work(1 * ms, 3 * ms);
  fake_platform p;
  const run_result r = run(p, true, 300, [base](int i, fake_platform &pl) {
    if (i == 100) {
      pl.extra_late = ms;
    }
    return base(i, pl);
  });
  const std::vector<double> gaps = r.intervals(0);
  CHECK(gaps[99] > period_240 + 0.5); // frame 100
  // Counted from that frame: no short frames to catch up.
  for (std::size_t i = 100; i < gaps.size(); ++i) {
    CHECK(std::abs(gaps[i] - period_240) <= 0.04);
  }
  CHECK_EQ(r.stats.late, 1u);
  CHECK(r.stats.late_max > 7 * ms / 10);
}

TEST_CASE("frame pacing: a coarse system timer costs one short spin a frame") {
  // timeGetTime at 15.625 ms steps (no process asked for a finer timer):
  // neither limiter can do better than the steps, and the pacer spins at
  // most 0.3 + 1 ms a frame before it falls back to upstream's wait.
  const int frames = 300;
  fake_platform p;
  p.interrupt = 156'250;
  const run_result r = run(p, true, frames, uniform_work(1 * ms, 3 * ms));
  fake_platform q;
  q.interrupt = 156'250;
  const run_result u = run(q, false, frames, uniform_work(1 * ms, 3 * ms));
  CHECK(std::abs(mean(r.intervals()) - mean(u.intervals())) < 1.5);
  CHECK(r.stats.tick_missed > 0u);
  CHECK(r.stats.upstream_waits > 0u);
  CHECK(r.stats.spin <= frames * (13 * ms / 10 + 10));
  CHECK(*std::max_element(r.waits.begin(), r.waits.end()) <
        fp::engine_max_passes);
}

TEST_CASE("frame pacing: low caps wait in steps of at most 50 ms") {
  fake_platform p;
  p.fps = 10;
  const run_result r = run(p, true, 30, uniform_work(2 * ms, 5 * ms));
  const std::vector<double> gaps = r.intervals(2);
  CHECK(lowest(gaps) >= 100.0 - 0.05);
  CHECK(highest(gaps) <= 100.0 + 0.1);
  CHECK(p.longest_sleep <= 50 * ms);
  CHECK_EQ(r.stats.upstream_waits, 0u);
}

TEST_CASE("frame pacing: the counters as the game log has them") {
  fp::counters c;
  c.frames = 7200;
  c.paced = 7100;
  c.late = 3;
  c.late_max = 1'234; // 123.4 us
  c.tick_waits = 12;
  c.tick_missed = 1;
  c.upstream_waits = 2;
  c.spin = 21'000'000; // 2.1 s
  CHECK_EQ(fp::counters_json(c, freq, 240),
           std::string("{\"frames\":7200,\"paced\":7100,\"late\":3,"
                       "\"late_max_us\":123,\"tick_waits\":12,"
                       "\"tick_missed\":1,\"upstream_waits\":2,"
                       "\"spin_us\":2100000,\"maxfps\":240}"));
}
