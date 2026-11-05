// Frame-time statistics and run comparisons (perf plan H1), against values
// worked out by hand; the t-distribution numbers were checked against
// tables and a numerical integration of the density.

#include "harness.hpp"

#include <cmath>
#include <vector>

#include <xerces_core/perf_stats.hpp>

using namespace xerces_core::perf;

namespace {
bool near(double actual, double expected, double tolerance = 1e-9) {
  return std::fabs(actual - expected) <= tolerance;
}

#define CHECK_NEAR(actual, expected, tolerance)                                \
  ::xerces_tests::check_equal(near((actual), (expected), (tolerance)), true,   \
                              #actual " ~= " #expected, __FILE__, __LINE__,    \
                              false)

// 100 frames: 98 of 4 ms, one of 20 ms (frame 50) and one of 60 ms (frame
// 80). Total 472 ms.
std::vector<double> two_hitches() {
  std::vector<double> dt(100, 4.0);
  dt[50] = 20.0;
  dt[80] = 60.0;
  return dt;
}
} // namespace

TEST_CASE("perf: percentiles interpolate between closest ranks (type 7)") {
  const std::vector<double> values{4, 1, 3, 2};
  CHECK_NEAR(percentile(values, 50), 2.5, 1e-12);
  CHECK_NEAR(percentile(values, 95), 3.85, 1e-12); // h = 2.85
  CHECK_NEAR(percentile(values, 0), 1.0, 1e-12);
  CHECK_NEAR(percentile(values, 100), 4.0, 1e-12);
  CHECK_NEAR(percentile({7.0}, 99), 7.0, 1e-12);
  CHECK(std::isnan(percentile({}, 50)));
}

TEST_CASE("perf: Student's t quantiles and distribution") {
  CHECK_NEAR(student_t_quantile(0.975, 5), 2.570581836, 1e-7);
  CHECK_NEAR(student_t_quantile(0.975, 10), 2.228138852, 1e-7);
  CHECK_NEAR(student_t_quantile(0.95, 5), 2.015048373, 1e-7);
  CHECK_NEAR(student_t_quantile(0.995, 3), 5.840909309, 1e-6);
  CHECK_NEAR(student_t_quantile(0.025, 5), -2.570581836, 1e-7);
  CHECK_NEAR(student_t_quantile(0.975, 1000), 1.962339, 1e-5);
  CHECK_NEAR(student_t_cdf(0, 7), 0.5, 1e-12);
  CHECK_NEAR(student_t_cdf(2.228138852, 10), 0.975, 1e-9);
  CHECK_NEAR(student_t_cdf(-2.570581836, 5), 0.025, 1e-9);
  // One degree of freedom is the Cauchy distribution: 0.5 + atan(t) / pi.
  CHECK_NEAR(student_t_cdf(1.5, 1), 0.5 + std::atan(1.5) / 3.14159265358979,
             1e-9);
  CHECK(std::isnan(student_t_quantile(1.0, 5)));
}

TEST_CASE("perf: a run's summary, worked by hand") {
  const auto dt = two_hitches();
  const frame_summary s = summarize_frames(dt);
  CHECK_EQ(s.frames, 100u);
  CHECK_NEAR(s.total_ms, 472.0, 1e-9);
  CHECK_NEAR(s.mean_ms, 4.72, 1e-12);
  CHECK_NEAR(s.avg_fps, 100000.0 / 472.0, 1e-9);
  CHECK_NEAR(s.min_ms, 4.0, 0);
  CHECK_NEAR(s.median_ms, 4.0, 1e-12);
  CHECK_NEAR(s.p95_ms, 4.0, 1e-12);
  CHECK_NEAR(s.p99_ms, 20.4, 1e-9);   // h = 98.01: 20 + 0.01 * 40
  CHECK_NEAR(s.p999_ms, 56.04, 1e-9); // h = 98.901: 20 + 0.901 * 40
  CHECK_NEAR(s.max_ms, 60.0, 0);
  // 1%: the worst ceil(1) frame, and 1000 / p99.
  CHECK_EQ(s.low_1.worst_frames, 1u);
  CHECK_NEAR(s.low_1.mean_of_worst_fps, 1000.0 / 60.0, 1e-9);
  CHECK_NEAR(s.low_1.percentile_fps, 1000.0 / 20.4, 1e-9);
  CHECK_EQ(s.low_01.worst_frames, 1u);
  CHECK_NEAR(s.low_01.mean_of_worst_fps, 1000.0 / 60.0, 1e-9);
  CHECK_NEAR(s.low_01.percentile_fps, 1000.0 / 56.04, 1e-9);
  // Both long frames are hitches (rolling median 4 ms): 20 > 8 and > 8.
  CHECK_EQ(s.hitches, 2u);
  CHECK_EQ(s.over_50ms, 1u);
  CHECK_EQ(s.over_100ms, 0u);
  CHECK_NEAR(s.hitch_ms, 80.0, 1e-9);
  CHECK_NEAR(s.stutter_pct, 8000.0 / 472.0, 1e-9);
  REQUIRE_EQ(s.hitch_list.size(), 2u);
  CHECK_EQ(s.hitch_list[0].index, 50u);
  CHECK_EQ(s.hitch_list[1].index, 80u);
  CHECK_NEAR(s.hitch_list[1].rolling_median_ms, 4.0, 1e-12);
  // Halves by time (236 ms each): frames 0-54 end by 236 ms, the rest after.
  CHECK_NEAR(s.first_half_mean_ms, 236.0 / 55.0, 1e-9);
  CHECK_NEAR(s.second_half_mean_ms, 236.0 / 45.0, 1e-9);
  CHECK_NEAR(s.drift_pct, 100.0 * (55.0 / 45.0 - 1.0), 1e-9);
  CHECK(s.drift_warning);
}

TEST_CASE("perf: hitch rules need both the ratio and the extra 4 ms") {
  // 2 ms frames: one of 7 ms is over both 2x the rolling median (4 ms) and
  // the median + 4 ms (6 ms); one of 5.5 ms is over 2x but not over 6 ms.
  std::vector<double> dt(61, 2.0);
  dt[20] = 7.0;
  dt[40] = 5.5;
  const frame_summary s = summarize_frames(dt);
  CHECK_EQ(s.hitches, 1u);
  REQUIRE_EQ(s.hitch_list.size(), 1u);
  CHECK_EQ(s.hitch_list[0].index, 20u);
  // A steady run does not drift.
  const frame_summary steady = summarize_frames(std::vector<double>(50, 5.0));
  CHECK_NEAR(steady.drift_pct, 0.0, 1e-12);
  CHECK(!steady.drift_warning);
  CHECK_EQ(steady.hitches, 0u);
  CHECK_NEAR(steady.sd_ms, 0.0, 1e-12);
  // Nothing to summarize.
  CHECK_EQ(summarize_frames({}).frames, 0u);
}

TEST_CASE("perf: lows from a truncated worst-first list match the full one") {
  // 1000 frames 1..1000 ms in a scrambled order.
  std::vector<double> dt;
  for (int i = 0; i < 1000; ++i) {
    dt.push_back(static_cast<double>((i * 7919) % 1000 + 1));
  }
  const frame_summary s = summarize_frames(dt);
  // The worst 1% are 991..1000 ms (mean 995.5); p99 = 990.01.
  CHECK_EQ(s.low_1.worst_frames, 10u);
  CHECK_NEAR(s.low_1.mean_of_worst_fps, 1000.0 / 995.5, 1e-12);
  CHECK_NEAR(s.p99_ms, 990.01, 1e-9);
  // 0.1% of 1000 frames is one frame, not two through rounding.
  CHECK_EQ(s.low_01.worst_frames, 1u);
  std::vector<double> worst;
  for (int v = 1000; v >= 985; --v) {
    worst.push_back(v);
  }
  const low_pair one = low_from_worst(worst, 1000, 1.0);
  CHECK_NEAR(one.mean_of_worst_fps, s.low_1.mean_of_worst_fps, 1e-12);
  CHECK_NEAR(one.percentile_fps, s.low_1.percentile_fps, 1e-12);
  const low_pair tenth = low_from_worst(worst, 1000, 0.1);
  CHECK_NEAR(tenth.percentile_fps, s.low_01.percentile_fps, 1e-12);
}

TEST_CASE("perf: runs as samples: CI, Welch and TOST, worked by hand") {
  const std::vector<double> a{10, 11, 12, 13, 14, 15};
  const std::vector<double> b{11, 12, 13, 14, 15, 16};
  const sample_stats sa = describe_sample(a);
  CHECK_EQ(sa.n, 6u);
  CHECK_NEAR(sa.mean, 12.5, 1e-12);
  CHECK_NEAR(sa.sd, std::sqrt(3.5), 1e-12);
  CHECK_NEAR(sa.median, 12.5, 1e-12);
  REQUIRE(sa.ci_low.has_value());
  CHECK_NEAR(*sa.ci_low, 10.536685693, 1e-6);
  CHECK_NEAR(*sa.ci_high, 14.463314307, 1e-6);

  const welch_result w = welch(a, b);
  REQUIRE(w.ok);
  CHECK_NEAR(w.diff, 1.0, 1e-12);
  CHECK_NEAR(w.diff_pct, 8.0, 1e-9);
  CHECK_NEAR(w.se, 1.0801234497, 1e-9);
  CHECK_NEAR(w.df, 10.0, 1e-9);
  CHECK_NEAR(w.ci_low, -1.406665023, 1e-6);
  CHECK_NEAR(w.ci_high, 3.406665023, 1e-6);
  CHECK_NEAR(w.p_value, 0.376341738, 1e-6);

  // A 10% margin (1.25) is narrower than the 90% interval: not equivalent.
  const tost_result narrow = tost(a, b, 10.0);
  REQUIRE(narrow.ok);
  CHECK_NEAR(narrow.margin, 1.25, 1e-12);
  CHECK_NEAR(narrow.ci_low, -0.957681761, 1e-6);
  CHECK_NEAR(narrow.ci_high, 2.957681761, 1e-6);
  CHECK_NEAR(narrow.p_lower, 0.031932585, 1e-6);
  CHECK_NEAR(narrow.p_upper, 0.410814285, 1e-6);
  CHECK(!narrow.equivalent);
  // 30% (3.75) holds the interval: equivalent.
  const tost_result wide = tost(a, b, 30.0);
  CHECK_NEAR(wide.p_lower, 0.000669990, 1e-7);
  CHECK_NEAR(wide.p_upper, 0.014531177, 1e-6);
  CHECK(wide.equivalent);

  // One run a side cannot be tested.
  const std::vector<double> one{12.0};
  CHECK(!welch(one, b).ok);
  CHECK(!tost(one, b, 1.0).ok);
  CHECK(!describe_sample(one).ci_low.has_value());
  // Identical runs: no spread, the difference is exact.
  const std::vector<double> same{5, 5, 5};
  const welch_result flat = welch(same, same);
  CHECK(flat.ok);
  CHECK_NEAR(flat.diff, 0.0, 0);
  CHECK_NEAR(flat.p_value, 1.0, 0);
  CHECK(tost(same, same, 1.0).equivalent);
}

TEST_CASE("perf: the pooled-lows bootstrap is seeded and pools correctly") {
  std::vector<std::vector<double>> a;
  std::vector<std::vector<double>> b;
  for (int run = 0; run < 4; ++run) {
    std::vector<double> frames(2000, 4.0 + 0.1 * run);
    frames[100 + run] = 30.0 + run;
    frames[900] = 12.0;
    a.push_back(frames);
    for (auto &f : frames) {
      f += 0.5;
    }
    b.push_back(frames);
  }
  const pooled_bootstrap first = bootstrap_pooled_lows(a, b, 300, 7);
  const pooled_bootstrap again = bootstrap_pooled_lows(a, b, 300, 7);
  REQUIRE(first.ok);
  REQUIRE_EQ(first.metrics.size(), 4u);
  for (std::size_t m = 0; m < 4; ++m) {
    CHECK_EQ(first.metrics[m].a.low, again.metrics[m].a.low);
    CHECK_EQ(first.metrics[m].diff.high, again.metrics[m].diff.high);
  }
  // The estimate is every run pooled once: the same as one long run.
  std::vector<double> pooled;
  for (const auto &run : a) {
    pooled.insert(pooled.end(), run.begin(), run.end());
  }
  const frame_summary all = summarize_frames(pooled);
  CHECK_EQ(first.metrics[0].name, std::string("low_1_mean_of_worst_fps"));
  CHECK_NEAR(first.metrics[0].a.estimate, all.low_1.mean_of_worst_fps, 1e-9);
  CHECK_NEAR(first.metrics[1].a.estimate, all.low_1.percentile_fps, 1e-9);
  CHECK_NEAR(first.metrics[2].a.estimate, all.low_01.mean_of_worst_fps, 1e-9);
  CHECK_NEAR(first.metrics[3].a.estimate, all.low_01.percentile_fps, 1e-9);
  for (const auto &m : first.metrics) {
    CHECK(m.a.low <= m.a.high);
    CHECK_NEAR(m.diff.estimate, m.b.estimate - m.a.estimate, 1e-9);
  }
  // B's frames are all 0.5 ms longer: its lows are lower.
  CHECK(first.metrics[1].diff.estimate < 0);
  CHECK(!bootstrap_pooled_lows({}, b).ok);
}
