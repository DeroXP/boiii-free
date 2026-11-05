#include "perf_stats.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <numeric>

namespace xerces_core::perf {
namespace {
constexpr double nan = std::numeric_limits<double>::quiet_NaN();

double mean_of(std::span<const double> values) {
  if (values.empty()) {
    return 0;
  }
  return std::accumulate(values.begin(), values.end(), 0.0) /
         static_cast<double>(values.size());
}

// Sample variance (n - 1); 0 for fewer than two values.
double variance_of(std::span<const double> values) {
  if (values.size() < 2) {
    return 0;
  }
  const double mean = mean_of(values);
  double sum = 0;
  for (const double v : values) {
    sum += (v - mean) * (v - mean);
  }
  return sum / static_cast<double>(values.size() - 1);
}

// The continued fraction of the regularized incomplete beta function
// (Lentz's method, as in Numerical Recipes' betacf).
double beta_fraction(double a, double b, double x) {
  constexpr int max_iterations = 500;
  constexpr double epsilon = 1e-15;
  constexpr double tiny = 1e-300;
  const double qab = a + b;
  const double qap = a + 1;
  const double qam = a - 1;
  double c = 1;
  double d = 1 - qab * x / qap;
  if (std::fabs(d) < tiny) {
    d = tiny;
  }
  d = 1 / d;
  double h = d;
  for (int m = 1; m <= max_iterations; ++m) {
    const double m2 = 2.0 * m;
    double aa = m * (b - m) * x / ((qam + m2) * (a + m2));
    d = 1 + aa * d;
    if (std::fabs(d) < tiny) {
      d = tiny;
    }
    c = 1 + aa / c;
    if (std::fabs(c) < tiny) {
      c = tiny;
    }
    d = 1 / d;
    h *= d * c;
    aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2));
    d = 1 + aa * d;
    if (std::fabs(d) < tiny) {
      d = tiny;
    }
    c = 1 + aa / c;
    if (std::fabs(c) < tiny) {
      c = tiny;
    }
    d = 1 / d;
    const double step = d * c;
    h *= step;
    if (std::fabs(step - 1) < epsilon) {
      break;
    }
  }
  return h;
}

// I_x(a, b).
double incomplete_beta(double a, double b, double x) {
  if (x <= 0) {
    return 0;
  }
  if (x >= 1) {
    return 1;
  }
  const double front =
      std::exp(std::lgamma(a + b) - std::lgamma(a) - std::lgamma(b) +
               a * std::log(x) + b * std::log1p(-x));
  if (x < (a + 1) / (a + b + 2)) {
    return front * beta_fraction(a, b, x) / a;
  }
  return 1 - front * beta_fraction(b, a, 1 - x) / b;
}

// ceil(total * pct / 100), at least 1 and at most total; the small
// tolerance keeps 1000 * 0.1% from becoming 2 frames through rounding.
std::size_t worst_count(std::size_t total, double pct) {
  const double exact = static_cast<double>(total) * pct / 100.0;
  auto count = static_cast<std::size_t>(std::ceil(exact - 1e-9));
  return std::clamp<std::size_t>(count, 1, std::max<std::size_t>(total, 1));
}

double median_of_window(std::vector<double> &window) {
  std::sort(window.begin(), window.end());
  return percentile_sorted(window, 50);
}

// splitmix64: small, fast, and the same sequence on every compiler (the
// standard distributions are not).
std::uint64_t next_random(std::uint64_t &state) {
  std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

// Each run's worst frames, worst first: enough of them that the worst 1% of
// any resample (at most every run, each drawn every time) is among them.
struct prepared_runs {
  std::vector<std::vector<double>> worst;
  std::vector<std::size_t> sizes;
};

prepared_runs prepare(const std::vector<std::vector<double>> &runs) {
  prepared_runs out;
  std::size_t longest = 0;
  for (const auto &run : runs) {
    longest = std::max(longest, run.size());
  }
  const std::size_t keep = worst_count(runs.size() * longest, 1.0) + 2;
  for (const auto &run : runs) {
    std::vector<double> copy = run;
    const std::size_t n = std::min(keep, copy.size());
    std::partial_sort(copy.begin(), copy.begin() + static_cast<long long>(n),
                      copy.end(), std::greater<>());
    copy.resize(n);
    out.worst.push_back(std::move(copy));
    out.sizes.push_back(run.size());
  }
  return out;
}

// The four pooled lows of the runs `picks` names: 1% and 0.1%, mean of the
// worst and percentile.
std::array<double, 4> pooled_lows(const prepared_runs &runs,
                                  const std::vector<std::size_t> &picks,
                                  std::vector<double> &buffer) {
  buffer.clear();
  std::size_t total = 0;
  for (const std::size_t pick : picks) {
    buffer.insert(buffer.end(), runs.worst[pick].begin(),
                  runs.worst[pick].end());
    total += runs.sizes[pick];
  }
  const std::size_t need = std::min(buffer.size(), worst_count(total, 1.0) + 2);
  std::partial_sort(buffer.begin(),
                    buffer.begin() + static_cast<long long>(need), buffer.end(),
                    std::greater<>());
  const std::span<const double> worst(buffer.data(), need);
  const low_pair one = low_from_worst(worst, total, 1.0);
  const low_pair tenth = low_from_worst(worst, total, 0.1);
  return {one.mean_of_worst_fps, one.percentile_fps, tenth.mean_of_worst_fps,
          tenth.percentile_fps};
}
} // namespace

double percentile_sorted(std::span<const double> sorted, double p) {
  if (sorted.empty()) {
    return nan;
  }
  const double h =
      static_cast<double>(sorted.size() - 1) * std::clamp(p, 0.0, 100.0) / 100;
  const auto low = static_cast<std::size_t>(std::floor(h));
  if (low + 1 >= sorted.size()) {
    return sorted.back();
  }
  return sorted[low] +
         (h - static_cast<double>(low)) * (sorted[low + 1] - sorted[low]);
}

double percentile(std::vector<double> values, double p) {
  std::sort(values.begin(), values.end());
  return percentile_sorted(values, p);
}

double student_t_cdf(double t, double df) {
  if (!(df > 0) || std::isnan(t)) {
    return nan;
  }
  if (std::isinf(t)) {
    return t > 0 ? 1.0 : 0.0;
  }
  const double tail = 0.5 * incomplete_beta(df / 2, 0.5, df / (df + t * t));
  return t > 0 ? 1 - tail : tail;
}

double student_t_quantile(double p, double df) {
  if (!(df > 0) || !(p > 0) || !(p < 1)) {
    return nan;
  }
  if (p == 0.5) {
    return 0;
  }
  if (p < 0.5) {
    return -student_t_quantile(1 - p, df);
  }
  double high = 1;
  while (student_t_cdf(high, df) < p && high < 1e12) {
    high *= 2;
  }
  double low = 0;
  for (int i = 0; i < 200 && high - low > 1e-13 * std::max(1.0, high); ++i) {
    const double mid = (low + high) / 2;
    (student_t_cdf(mid, df) < p ? low : high) = mid;
  }
  return (low + high) / 2;
}

low_pair low_from_worst(std::span<const double> worst_first, std::size_t total,
                        double pct) {
  low_pair out;
  if (total == 0 || worst_first.empty()) {
    return out;
  }
  const std::size_t k = std::min(worst_count(total, pct), worst_first.size());
  double sum = 0;
  for (std::size_t i = 0; i < k; ++i) {
    sum += worst_first[i];
  }
  out.worst_frames = k;
  out.mean_of_worst_fps = sum > 0 ? 1000.0 * static_cast<double>(k) / sum : 0;

  // The (100 - pct)th percentile, type 7, read from the worst-first end:
  // ascending rank r is worst_first[total - 1 - r].
  const double h = static_cast<double>(total - 1) * (100.0 - pct) / 100.0;
  const auto low = static_cast<std::size_t>(std::floor(h));
  const auto at = [&](std::size_t ascending) {
    const std::size_t index =
        std::min(total - 1 - ascending, worst_first.size() - 1);
    return worst_first[index];
  };
  double value = at(low);
  if (low + 1 < total) {
    value += (h - static_cast<double>(low)) * (at(low + 1) - at(low));
  }
  out.percentile_fps = value > 0 ? 1000.0 / value : 0;
  return out;
}

frame_summary summarize_frames(std::span<const double> dt_ms,
                               const frame_options &options) {
  frame_summary out;
  const std::size_t n = dt_ms.size();
  if (n == 0) {
    return out;
  }
  out.frames = n;
  out.total_ms = std::accumulate(dt_ms.begin(), dt_ms.end(), 0.0);
  out.mean_ms = out.total_ms / static_cast<double>(n);
  out.avg_fps = out.mean_ms > 0 ? 1000.0 / out.mean_ms : 0;
  out.sd_ms = std::sqrt(variance_of(dt_ms));

  std::vector<double> sorted(dt_ms.begin(), dt_ms.end());
  std::sort(sorted.begin(), sorted.end());
  out.min_ms = sorted.front();
  out.max_ms = sorted.back();
  out.median_ms = percentile_sorted(sorted, 50);
  out.p95_ms = percentile_sorted(sorted, 95);
  out.p99_ms = percentile_sorted(sorted, 99);
  out.p999_ms = percentile_sorted(sorted, 99.9);
  std::vector<double> worst_first(sorted.rbegin(), sorted.rend());
  out.low_1 = low_from_worst(worst_first, n, 1.0);
  out.low_01 = low_from_worst(worst_first, n, 0.1);

  // Hitches against the rolling median of the frames around each one.
  const std::size_t half = std::max<std::size_t>(options.rolling_window, 1) / 2;
  std::vector<double> window;
  std::vector<hitch> all;
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t from = i > half ? i - half : 0;
    const std::size_t to = std::min(n, i + half + 1);
    window.assign(dt_ms.begin() + static_cast<long long>(from),
                  dt_ms.begin() + static_cast<long long>(to));
    const double rolling = median_of_window(window);
    const double dt = dt_ms[i];
    if (dt > options.hitch_ratio * rolling &&
        dt > rolling + options.hitch_extra_ms) {
      ++out.hitches;
      out.hitch_ms += dt;
      all.push_back({i, dt, rolling});
    }
    out.over_50ms += dt > 50.0 ? 1 : 0;
    out.over_100ms += dt > 100.0 ? 1 : 0;
  }
  out.stutter_pct = out.total_ms > 0 ? 100.0 * out.hitch_ms / out.total_ms : 0;
  std::stable_sort(all.begin(), all.end(), [](const hitch &a, const hitch &b) {
    return a.dt_ms > b.dt_ms;
  });
  if (all.size() > options.hitches_listed) {
    all.resize(options.hitches_listed);
  }
  std::sort(all.begin(), all.end(),
            [](const hitch &a, const hitch &b) { return a.index < b.index; });
  out.hitch_list = std::move(all);

  // Drift between the two halves of the run, by time.
  const double middle = out.total_ms / 2;
  double end = 0;
  double first_sum = 0;
  std::size_t first_count = 0;
  for (std::size_t i = 0; i < n; ++i) {
    end += dt_ms[i];
    if (end <= middle) {
      first_sum += dt_ms[i];
      ++first_count;
    }
  }
  const std::size_t second_count = n - first_count;
  if (first_count > 0 && second_count > 0) {
    out.first_half_mean_ms = first_sum / static_cast<double>(first_count);
    out.second_half_mean_ms =
        (out.total_ms - first_sum) / static_cast<double>(second_count);
    out.drift_pct = 100.0 * (out.second_half_mean_ms - out.first_half_mean_ms) /
                    out.first_half_mean_ms;
    out.drift_warning = std::fabs(out.drift_pct) > options.drift_warn_pct;
  }
  return out;
}

sample_stats describe_sample(std::span<const double> values,
                             double confidence) {
  sample_stats out;
  out.n = values.size();
  if (values.empty()) {
    return out;
  }
  out.mean = mean_of(values);
  out.sd = std::sqrt(variance_of(values));
  std::vector<double> sorted(values.begin(), values.end());
  std::sort(sorted.begin(), sorted.end());
  out.median = percentile_sorted(sorted, 50);
  out.min = sorted.front();
  out.max = sorted.back();
  if (out.n >= 2) {
    const double t = student_t_quantile((1 + confidence) / 2,
                                        static_cast<double>(out.n - 1));
    const double half = t * out.sd / std::sqrt(static_cast<double>(out.n));
    out.ci_low = out.mean - half;
    out.ci_high = out.mean + half;
  }
  return out;
}

namespace {
struct welch_parts {
  double diff = 0;
  double se = 0;
  double df = 0;
  double mean_a = 0;
};

welch_parts welch_core(std::span<const double> a, std::span<const double> b) {
  welch_parts out;
  out.mean_a = mean_of(a);
  out.diff = mean_of(b) - out.mean_a;
  const double va = variance_of(a) / static_cast<double>(a.size());
  const double vb = variance_of(b) / static_cast<double>(b.size());
  out.se = std::sqrt(va + vb);
  const double denominator = va * va / static_cast<double>(a.size() - 1) +
                             vb * vb / static_cast<double>(b.size() - 1);
  out.df = denominator > 0 ? (va + vb) * (va + vb) / denominator
                           : static_cast<double>(a.size() + b.size() - 2);
  return out;
}
} // namespace

welch_result welch(std::span<const double> a, std::span<const double> b,
                   double confidence) {
  welch_result out;
  if (a.size() < 2 || b.size() < 2) {
    return out;
  }
  out.ok = true;
  const welch_parts parts = welch_core(a, b);
  out.diff = parts.diff;
  out.diff_pct = parts.mean_a != 0 ? 100.0 * parts.diff / parts.mean_a : 0;
  out.se = parts.se;
  out.df = parts.df;
  if (parts.se == 0) {
    out.ci_low = out.ci_high = parts.diff;
    out.p_value = parts.diff == 0 ? 1.0 : 0.0;
    return out;
  }
  const double t = student_t_quantile((1 + confidence) / 2, parts.df);
  out.ci_low = parts.diff - t * parts.se;
  out.ci_high = parts.diff + t * parts.se;
  out.p_value =
      2 * (1 - student_t_cdf(std::fabs(parts.diff) / parts.se, parts.df));
  return out;
}

tost_result tost(std::span<const double> a, std::span<const double> b,
                 double margin_pct, double alpha) {
  tost_result out;
  out.margin_pct = margin_pct;
  if (a.size() < 2 || b.size() < 2) {
    return out;
  }
  out.ok = true;
  const welch_parts parts = welch_core(a, b);
  out.margin = std::fabs(parts.mean_a) * margin_pct / 100.0;
  if (parts.se == 0) {
    out.ci_low = out.ci_high = parts.diff;
    out.equivalent = std::fabs(parts.diff) < out.margin;
    out.p_lower = out.p_upper = out.equivalent ? 0.0 : 1.0;
    return out;
  }
  const double t = student_t_quantile(1 - alpha, parts.df);
  out.ci_low = parts.diff - t * parts.se;
  out.ci_high = parts.diff + t * parts.se;
  out.p_lower =
      1 - student_t_cdf((parts.diff + out.margin) / parts.se, parts.df);
  out.p_upper = student_t_cdf((parts.diff - out.margin) / parts.se, parts.df);
  out.equivalent = out.p_lower < alpha && out.p_upper < alpha;
  return out;
}

pooled_bootstrap
bootstrap_pooled_lows(const std::vector<std::vector<double>> &runs_a,
                      const std::vector<std::vector<double>> &runs_b,
                      std::size_t iterations, std::uint64_t seed) {
  pooled_bootstrap out;
  out.iterations = iterations;
  out.seed = seed;
  const auto has_frames = [](const std::vector<std::vector<double>> &runs) {
    return std::ranges::any_of(runs, [](const auto &r) { return !r.empty(); });
  };
  if (!has_frames(runs_a) || !has_frames(runs_b)) {
    return out;
  }
  out.ok = true;
  const prepared_runs a = prepare(runs_a);
  const prepared_runs b = prepare(runs_b);
  std::vector<double> buffer;

  std::vector<std::size_t> all_a(runs_a.size());
  std::vector<std::size_t> all_b(runs_b.size());
  std::iota(all_a.begin(), all_a.end(), std::size_t{0});
  std::iota(all_b.begin(), all_b.end(), std::size_t{0});
  const auto estimate_a = pooled_lows(a, all_a, buffer);
  const auto estimate_b = pooled_lows(b, all_b, buffer);

  std::array<std::vector<double>, 4> samples_a;
  std::array<std::vector<double>, 4> samples_b;
  std::array<std::vector<double>, 4> samples_diff;
  std::uint64_t state = seed;
  std::vector<std::size_t> picks_a(runs_a.size());
  std::vector<std::size_t> picks_b(runs_b.size());
  for (std::size_t it = 0; it < iterations; ++it) {
    for (auto &pick : picks_a) {
      pick = static_cast<std::size_t>(next_random(state) % runs_a.size());
    }
    for (auto &pick : picks_b) {
      pick = static_cast<std::size_t>(next_random(state) % runs_b.size());
    }
    const auto lows_a = pooled_lows(a, picks_a, buffer);
    const auto lows_b = pooled_lows(b, picks_b, buffer);
    for (std::size_t m = 0; m < 4; ++m) {
      samples_a[m].push_back(lows_a[m]);
      samples_b[m].push_back(lows_b[m]);
      samples_diff[m].push_back(lows_b[m] - lows_a[m]);
    }
  }
  const char *names[4] = {"low_1_mean_of_worst_fps", "low_1_percentile_fps",
                          "low_01_mean_of_worst_fps", "low_01_percentile_fps"};
  const auto interval = [](double estimate, std::vector<double> &samples) {
    bootstrap_interval result;
    result.estimate = estimate;
    std::sort(samples.begin(), samples.end());
    result.low = samples.empty() ? estimate : percentile_sorted(samples, 2.5);
    result.high = samples.empty() ? estimate : percentile_sorted(samples, 97.5);
    return result;
  };
  for (std::size_t m = 0; m < 4; ++m) {
    pooled_low_metric metric;
    metric.name = names[m];
    metric.a = interval(estimate_a[m], samples_a[m]);
    metric.b = interval(estimate_b[m], samples_b[m]);
    metric.diff = interval(estimate_b[m] - estimate_a[m], samples_diff[m]);
    out.metrics.push_back(std::move(metric));
  }
  return out;
}
} // namespace xerces_core::perf
