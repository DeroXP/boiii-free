#pragma once

// Frame-time statistics and run comparisons for the benchmark (DESIGN.md
// 12.2, 12.3; perf plan H1). Headless and pure, so every number is tested on
// synthetic data (src/xerces_tests/test_perf_stats.cpp).
//
// Definitions, used everywhere a number is printed:
// - Percentiles interpolate linearly between the closest ranks (Hyndman-Fan
//   type 7, numpy's default): rank h = (n - 1) * p / 100 in sorted order.
// - Mean frame time is total time / frames; average FPS is 1000 / that.
// - An "x% low" is given both ways, each labelled: 1000 / the mean of the
//   worst x% of frames (ceil(n * x / 100) frames, at least one), and
//   1000 / the (100 - x)th percentile frame time.
// - A hitch is a frame longer than both 2x the rolling median of the 31
//   frames centred on it (fewer at the ends of the run) and that median plus
//   4 ms. Frames over 50 ms and over 100 ms are counted on their own.
// - Stutter time is the time spent in hitch frames, as a percentage of the
//   run's total time.
// - Drift compares the two halves of the run by time (a frame belongs to the
//   first half when it ends by half the total): (second - first) / first, as
//   a percentage of the first half's mean frame time. Above 2% the run was
//   not steady (heat, a background task, a scene that changed).
//
// Comparing runs treats each run as one sample (frames within a run are not
// independent): t-based confidence intervals, Welch's difference, and TOST
// equivalence with margins in percent of A's mean. The pooled lows come from
// a cluster bootstrap (whole runs resampled) with a fixed seed; they describe
// the data and are not a test.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace xerces_core::perf {
// --- Percentiles and Student's t ---------------------------------------
// `sorted` ascending; p in [0, 100]. NaN for no values.
double percentile_sorted(std::span<const double> sorted, double p);
double percentile(std::vector<double> values, double p);

// Student's t distribution with `df` degrees of freedom (df > 0, fractional
// allowed for Welch): P(T <= t), and the t with P(T <= t) = p.
double student_t_cdf(double t, double df);
double student_t_quantile(double p, double df);

// --- One run -----------------------------------------------------------
struct frame_options {
  std::size_t rolling_window = 31; // frames in the rolling median
  double hitch_ratio = 2.0;        // longer than ratio x the rolling median
  double hitch_extra_ms = 4.0;     // and than the rolling median + this
  double drift_warn_pct = 2.0;
  std::size_t hitches_listed = 20; // the longest hitches kept in `hitch_list`
};

// One "x% low", both ways.
struct low_pair {
  double mean_of_worst_fps = 0; // 1000 / mean of the worst x% of frames
  double percentile_fps = 0;    // 1000 / the (100 - x)th percentile
  std::size_t worst_frames = 0; // frames the first one averages
};

struct hitch {
  std::size_t index = 0; // frame index in the series given
  double dt_ms = 0;
  double rolling_median_ms = 0;
};

struct frame_summary {
  std::size_t frames = 0;
  double total_ms = 0;
  double mean_ms = 0; // total / frames
  double avg_fps = 0; // 1000 / mean_ms
  double sd_ms = 0;   // sample standard deviation of the frame times
  double min_ms = 0;
  double median_ms = 0;
  double p95_ms = 0;
  double p99_ms = 0;
  double p999_ms = 0;
  double max_ms = 0;
  low_pair low_1;  // 1% low
  low_pair low_01; // 0.1% low
  std::size_t hitches = 0;
  std::size_t over_50ms = 0;
  std::size_t over_100ms = 0;
  double hitch_ms = 0;    // time in hitch frames
  double stutter_pct = 0; // 100 * hitch_ms / total_ms
  double first_half_mean_ms = 0;
  double second_half_mean_ms = 0;
  double drift_pct = 0;
  bool drift_warning = false;
  std::vector<hitch> hitch_list; // the longest, in frame order
};

// Frame times in ms, in order. Non-finite and non-positive values must be
// filtered out by the caller (the importers do).
frame_summary summarize_frames(std::span<const double> dt_ms,
                               const frame_options &options = {});

// "x% low" on frames already sorted in descending order (the worst first),
// of which only the first entries may be given: `total` is the number of
// frames the list was taken from. Used for pooled lows without sorting every
// frame of every run again.
low_pair low_from_worst(std::span<const double> worst_first, std::size_t total,
                        double pct);

// --- Runs as samples -----------------------------------------------------
struct sample_stats {
  std::size_t n = 0;
  double mean = 0;
  double sd = 0; // sample sd; 0 when n < 2
  double median = 0;
  double min = 0;
  double max = 0;
  std::optional<double> ci_low; // t-based, two-sided; nullopt when n < 2
  std::optional<double> ci_high;
};
sample_stats describe_sample(std::span<const double> values,
                             double confidence = 0.95);

struct welch_result {
  double diff = 0;     // mean(b) - mean(a)
  double diff_pct = 0; // diff / mean(a) * 100
  double se = 0;
  double df = 0;
  double ci_low = 0; // two-sided, at `confidence`
  double ci_high = 0;
  double p_value = 1; // two-sided Welch t test
  bool ok = false;    // both sides have at least two runs
};
welch_result welch(std::span<const double> a, std::span<const double> b,
                   double confidence = 0.95);

// Two one-sided tests: B is equivalent to A when the (1 - 2 alpha)
// confidence interval of mean(b) - mean(a) lies inside +-margin, margin
// being margin_pct percent of mean(a).
struct tost_result {
  double margin_pct = 0;
  double margin = 0; // in the metric's unit
  double ci_low = 0; // the (1 - 2 alpha) interval of the difference
  double ci_high = 0;
  double p_lower = 1; // H0: diff <= -margin
  double p_upper = 1; // H0: diff >= +margin
  bool equivalent = false;
  bool ok = false; // both sides have at least two runs
};
tost_result tost(std::span<const double> a, std::span<const double> b,
                 double margin_pct, double alpha = 0.05);

// --- Pooled lows (descriptive) -----------------------------------------
struct bootstrap_interval {
  double estimate = 0; // on every run pooled once
  double low = 0;      // 2.5th percentile of the resamples
  double high = 0;     // 97.5th
};
struct pooled_low_metric {
  std::string name; // e.g. "low_01_mean_of_worst_fps"
  bootstrap_interval a;
  bootstrap_interval b;
  bootstrap_interval diff; // b - a
};
struct pooled_bootstrap {
  std::size_t iterations = 0;
  std::uint64_t seed = 0;
  std::vector<pooled_low_metric> metrics;
  bool ok = false; // both sides have frames
};
inline constexpr std::uint64_t default_bootstrap_seed = 20261105;
// Each run's frame times; whole runs are drawn with replacement on each side,
// their frames pooled and the 1% and 0.1% lows (both ways) taken.
pooled_bootstrap
bootstrap_pooled_lows(const std::vector<std::vector<double>> &runs_a,
                      const std::vector<std::vector<double>> &runs_b,
                      std::size_t iterations = 2000,
                      std::uint64_t seed = default_bootstrap_seed);
} // namespace xerces_core::perf
