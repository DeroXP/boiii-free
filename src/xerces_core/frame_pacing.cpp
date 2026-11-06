#include "frame_pacing.hpp"

#include <algorithm>

#include "launch.hpp"

namespace xerces_core::frame_pacing {
bool requested(const std::vector<std::string> &args) {
  return has_launch_flag(args, flag) ||
         has_launch_flag(args, "-" + std::string(flag));
}

int engine_target_ms(const int maxfps) {
  // test ebx, ebx; jle (keep 1); mov eax, 1000; idiv ebx; cmove r15d (1)
  if (maxfps <= 0) {
    return 1;
  }
  const int target = 1000 / maxfps;
  return target == 0 ? 1 : target;
}

bool engine_pass(const std::int32_t now_ms, std::int32_t &last_ms,
                 const int target_ms) {
  // cmp eax, edx; cmovs edx, eax: the sign of the wrapped difference.
  const auto wrapped = [](std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) -
                                     static_cast<std::uint32_t>(b));
  };
  if (wrapped(now_ms, last_ms) < 0) {
    last_ms = now_ms;
  }
  return wrapped(now_ms, last_ms) >= target_ms;
}

timing timing_for(const std::int64_t freq) {
  timing t;
  t.freq = freq;
  t.spin = freq * 3 / 10'000;
  t.tick_spin = freq / 1'000;
  t.max_wait = freq * 50 / 1'000;
  t.late = freq / 10'000;
  return t;
}

std::int64_t period_ticks(const int maxfps, const std::int64_t freq) {
  if (maxfps <= 0 || freq <= 0) {
    return 0;
  }
  return (freq + maxfps / 2) / maxfps;
}

std::int64_t next_deadline(const std::int64_t released, const int maxfps,
                           const timing &t) {
  const std::int64_t period = period_ticks(maxfps, t.freq);
  if (released <= 0 || period <= 0) {
    return 0;
  }
  return released + period;
}

wait_step step_towards(const std::int64_t deadline, const std::int64_t now,
                       const timing &t) {
  if (deadline <= now) {
    return {};
  }
  wait_step step;
  step.until = std::min(deadline, now + t.max_wait);
  step.sleep_until = step.until == deadline ? deadline - t.spin : step.until;
  return step;
}

std::string counters_json(const counters &c, const std::int64_t freq,
                          const int maxfps) {
  const auto us = [freq](std::int64_t ticks) {
    return freq > 0 ? ticks * 1'000'000 / freq : 0;
  };
  return "{\"frames\":" + std::to_string(c.frames) +
         ",\"paced\":" + std::to_string(c.paced) +
         ",\"late\":" + std::to_string(c.late) +
         ",\"late_max_us\":" + std::to_string(us(c.late_max)) +
         ",\"tick_waits\":" + std::to_string(c.tick_waits) +
         ",\"tick_missed\":" + std::to_string(c.tick_missed) +
         ",\"upstream_waits\":" + std::to_string(c.upstream_waits) +
         ",\"spin_us\":" + std::to_string(us(c.spin)) +
         ",\"maxfps\":" + std::to_string(maxfps) + "}";
}
} // namespace xerces_core::frame_pacing
