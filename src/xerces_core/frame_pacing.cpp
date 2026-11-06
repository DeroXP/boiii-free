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

bool paces(const int maxfps) {
  return maxfps > 0 && engine_target_ms(maxfps) <= engine_max_passes;
}

timing timing_for(const std::int64_t freq) {
  timing t;
  t.freq = freq;
  t.spin = freq * 3 / 10'000;
  t.tick_spin = freq / 1'000;
  t.release_bin = std::max<std::int64_t>(freq * release_bin_us / 1'000'000, 1);
  return t;
}

std::int64_t period_ticks(const int maxfps, const std::int64_t freq) {
  if (!paces(maxfps) || freq <= 0) {
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
  step.until = deadline;
  step.sleep_until = deadline - t.spin;
  return step;
}

std::uint64_t released_count(const counters &c) {
  std::uint64_t n = 0;
  for (const std::uint32_t v : c.released) {
    n += v;
  }
  return n;
}

std::uint64_t released_under(const counters &c, const int us) {
  const int bins = std::clamp(us / release_bin_us, 0, release_bins);
  std::uint64_t n = 0;
  for (int i = 0; i < bins; ++i) {
    n += c.released[static_cast<std::size_t>(i)];
  }
  return n;
}

std::int64_t released_percentile_us(const counters &c, const double share,
                                    const std::int64_t freq) {
  const std::uint64_t total = released_count(c);
  if (total == 0) {
    return 0;
  }
  // The smallest bin whose running count reaches the share.
  const double wanted = share * static_cast<double>(total);
  std::uint64_t n = 0;
  for (int i = 0; i < release_bins; ++i) {
    n += c.released[static_cast<std::size_t>(i)];
    if (static_cast<double>(n) >= wanted) {
      if (i == release_bins - 1) {
        break; // past the bins: the latest one says more
      }
      return static_cast<std::int64_t>(i + 1) * release_bin_us;
    }
  }
  return freq > 0 ? c.released_max * 1'000'000 / freq : 0;
}

std::string counters_json(const counters &c, const std::int64_t freq,
                          const int maxfps) {
  const auto us = [freq](std::int64_t ticks) {
    return freq > 0 ? ticks * 1'000'000 / freq : 0;
  };
  return "{\"frames\":" + std::to_string(c.frames) +
         ",\"paced\":" + std::to_string(c.paced) +
         ",\"released_under_100us\":" + std::to_string(released_under(c, 100)) +
         ",\"released_p99_us\":" +
         std::to_string(released_percentile_us(c, 0.99, freq)) +
         ",\"released_max_us\":" + std::to_string(us(c.released_max)) +
         ",\"tick_waits\":" + std::to_string(c.tick_waits) +
         ",\"tick_missed\":" + std::to_string(c.tick_missed) +
         ",\"upstream_waits\":" + std::to_string(c.upstream_waits) +
         ",\"timer_errors\":" + std::to_string(c.timer_errors) +
         ",\"spin_us\":" + std::to_string(us(c.spin)) +
         ",\"maxfps\":" + std::to_string(maxfps) + "}";
}
} // namespace xerces_core::frame_pacing
