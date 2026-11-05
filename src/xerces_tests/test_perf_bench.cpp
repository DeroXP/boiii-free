// The pure parts of the game's performance instrumentation (DESIGN.md 12.2):
// the probe's CSV files, the run's meta file, XERCES_BENCH_AUTO, where bench
// output may go, and the perf.* log events' JSON.

#include "harness.hpp"

#include <string>
#include <vector>

#include <xerces_core/json.hpp>
#include <xerces_core/perf_bench.hpp>

using namespace xerces_core::bench;

namespace {
// A 10 MHz clock, as Windows 10 and 11 report QueryPerformanceFrequency.
constexpr std::int64_t mhz10 = 10'000'000;

std::vector<std::string> lines(const std::string &text) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start < text.size()) {
    const auto end = text.find('\n', start);
    out.push_back(text.substr(start, end - start));
    start = end == std::string::npos ? text.size() : end + 1;
  }
  return out;
}

bool parses(const std::string &text) {
  return xerces_core::json::parse(text).ok;
}
} // namespace

TEST_CASE("bench: frames.csv has the columns and times from the run start") {
  const run_clock clock{1'000'000, mhz10};
  std::vector<frame_sample> frames(3);
  frames[0] = {1'000'000 + 41'667, 12'000, 3, 7, frame_flag::sync_interval};
  frames[1] = {1'000'000 + 83'334, 10'000, 65535, 23, 0};
  frames[2] = {1'000'000 + 125'001, 0, 0, 0,
               frame_flag::overlay_drew | frame_flag::unfocused};
  const auto rows = lines(frames_csv(frames, 1'000'000, clock, 5));
  REQUIRE_EQ(rows.size(), 4u);
  CHECK_EQ(rows[0],
           std::string("i,t_ms,dt_ms,present_ms,overlay_us,cpu,flags"));
  CHECK_EQ(rows[1], std::string("5,4.1667,4.1667,1.2000,3,7,2"));
  CHECK_EQ(rows[2], std::string("6,8.3334,4.1667,1.0000,65535,23,0"));
  CHECK_EQ(rows[3], std::string("7,12.5001,4.1667,0.0000,0,0,9"));
  // Without the frame before the run, the first dt is left empty.
  const auto first = lines(frames_csv({frames.data(), 1}, std::nullopt, clock));
  CHECK_EQ(first[1], std::string("0,4.1667,,1.2000,3,7,2"));
}

TEST_CASE("bench: main.csv counts main-thread frames") {
  const run_clock clock{0, mhz10};
  const std::vector<std::int64_t> stamps{100'000, 260'000};
  const auto rows = lines(main_csv(stamps, std::nullopt, clock));
  REQUIRE_EQ(rows.size(), 3u);
  CHECK_EQ(rows[0], std::string("i,t_ms,dt_ms"));
  CHECK_EQ(rows[1], std::string("0,10.0000,"));
  CHECK_EQ(rows[2], std::string("1,26.0000,16.0000"));
}

TEST_CASE("bench: XERCES_BENCH_AUTO") {
  const auto menu = parse_auto_run("menu:10:60");
  REQUIRE(menu.has_value());
  CHECK(menu->when == auto_run::trigger::menu);
  CHECK_EQ(menu->start_s, 10.0);
  CHECK_EQ(menu->length_s, 60.0);
  const auto ingame = parse_auto_run("ingame:2.5:0.5");
  REQUIRE(ingame.has_value());
  CHECK(ingame->when == auto_run::trigger::ingame);
  CHECK_EQ(ingame->start_s, 2.5);
  for (const char *bad :
       {"", "menu", "menu:10", "menu:10:60:1", "lobby:1:2", "menu:-1:60",
        "menu:10:0", "menu:10:3601", "menu:x:60", "menu:10:60s", "MENU:1:2"}) {
    CHECK(!parse_auto_run(bad).has_value());
  }
}

TEST_CASE("bench: run names are safe file names") {
  CHECK_EQ(run_name("zm_ccube-idle_1", "x"), std::string("zm_ccube-idle_1"));
  CHECK_EQ(run_name("..\\..\\evil/run", "x"), std::string("evilrun"));
  CHECK_EQ(run_name("", "20261105-120000-manual"),
           std::string("20261105-120000-manual"));
  CHECK_EQ(run_name("....", "fallback"), std::string("fallback"));
  CHECK_EQ(run_name("trailing...", "x"), std::string("trailing"));
  CHECK_EQ(run_name("con", "x"), std::string("run-con"));
  CHECK_EQ(run_name("Nul.idle", "x"), std::string("run-Nul.idle"));
  CHECK_EQ(run_name("COM7", "x"), std::string("run-COM7"));
  CHECK_EQ(run_name("COMPARE", "x"), std::string("COMPARE"));
  CHECK_EQ(run_name(std::string(100, 'a'), "x").size(), 64u);
}

TEST_CASE("bench: output never goes under AppData or the game folder") {
  const std::vector<std::filesystem::path> forbidden{
      "C:\\Users\\Ginld\\AppData",
      "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Call of Duty Black "
      "Ops III",
      "C:\\Program Files (x86)\\Steam\\steamapps\\workshop", ""};
  CHECK(output_allowed("C:\\Users\\Ginld\\XercesHome-bench\\bench", forbidden));
  CHECK(output_allowed("C:\\Users\\Ginld\\XercesDev\\bench\\run1", forbidden));
  CHECK(!output_allowed("C:\\Users\\Ginld\\AppData\\Local\\Xerces\\bench",
                        forbidden));
  CHECK(!output_allowed("c:/users/ginld/appdata/roaming/x", forbidden));
  CHECK(!output_allowed("C:\\Users\\Ginld\\AppData", forbidden));
  CHECK(!output_allowed("C:\\Program Files (x86)\\Steam\\steamapps\\common\\"
                        "Call of Duty Black Ops III\\bench",
                        forbidden));
  CHECK(!output_allowed(
      "C:\\Program Files (x86)\\Steam\\steamapps\\workshop\\content\\311210",
      forbidden));
  // Whole components only: a sibling whose name starts the same is fine.
  CHECK(output_allowed("C:\\Users\\Ginld\\AppDataBench", forbidden));
  // ".." cannot climb into a forbidden folder unseen.
  CHECK(!output_allowed("C:\\Users\\Ginld\\XercesHome\\..\\AppData\\Local",
                        forbidden));
  // Relative paths would land in the game's working folder.
  CHECK(!output_allowed("bench", forbidden));
  CHECK(!output_allowed("", forbidden));
}

TEST_CASE("bench: perf.swapchain names the swap effect") {
  swapchain_info info;
  info.has_desc = true;
  info.width = 3840;
  info.height = 2160;
  info.refresh_numerator = 240000;
  info.refresh_denominator = 1000;
  info.buffer_count = 2;
  info.swap_effect = 0;
  info.flags = 2;
  info.windowed = true;
  info.swapchain1 = true;
  const std::string text = swapchain_json(info);
  CHECK(parses(text));
  CHECK(text.find("\"swap_effect_name\":\"discard\"") != std::string::npos);
  CHECK(text.find("\"refresh_hz\":240.000") != std::string::npos);
  CHECK(text.find("\"flags\":\"0x2\"") != std::string::npos);
  CHECK(text.find("\"swapchain2\":false") != std::string::npos);
  CHECK_EQ(swap_effect_name(4), std::string_view("flip_discard"));
  CHECK_EQ(swap_effect_name(2), std::string_view("unknown"));
  info.refresh_denominator = 0;
  CHECK(swapchain_json(info).find("\"refresh_hz\":null") != std::string::npos);
}

TEST_CASE("bench: meta.json is valid JSON with the run's clock") {
  run_meta meta;
  meta.run = "idle";
  meta.kind = "manual";
  meta.started_utc = "2026-11-05T12:00:00.000Z";
  meta.qpc_frequency = mhz10;
  meta.t0_qpc = 50'000'000;
  meta.t1_qpc = 650'000'000;
  meta.frames = 14400;
  meta.dvars = {{"com_maxfps", "240"},
                {"r_missing", std::nullopt},
                {"odd", std::string("a\"b\xff")}};
  const std::string text = run_meta_json(meta);
  REQUIRE(parses(text));
  const auto parsed = xerces_core::json::parse(text);
  const auto &doc = parsed.document;
  CHECK_EQ(std::string(doc["schema"].GetString()),
           std::string("xerces.bench/1"));
  CHECK_EQ(doc["duration_ms"].GetDouble(), 60000.0);
  CHECK_EQ(doc["t0_qpc"].GetInt64(), std::int64_t{50'000'000});
  CHECK(doc["swapchain"].IsNull());
  CHECK(doc["dvars"]["r_missing"].IsNull());
  CHECK_EQ(std::string(doc["dvars"]["com_maxfps"].GetString()),
           std::string("240"));
  meta.swapchain = swapchain_info{};
  CHECK(parses(run_meta_json(meta)));
}

TEST_CASE("bench: the written-run summary") {
  const run_clock clock{0, mhz10};
  std::vector<frame_sample> frames(4);
  for (std::size_t i = 0; i < frames.size(); ++i) {
    frames[i].qpc = std::int64_t(i + 1) * 40'000; // every 4 ms
  }
  frames[3].qpc = 3 * 40'000 + 250'000; // one 25 ms hitch
  const frames_summary s = summarize(frames, 0, clock, 400'000);
  CHECK_EQ(s.frames, 4u);
  CHECK_EQ(s.duration_ms, 40.0);
  CHECK_EQ(s.mean_fps, 100.0);
  CHECK_EQ(s.max_dt_ms, 25.0);
}

TEST_CASE("bench: perf.startup and perf.hooks") {
  startup_marks marks;
  marks.first_detour_ms = 14300.25;
  marks.frontend_ms = 30123.5;
  marks.detours = 412;
  const std::string startup = startup_json(marks);
  CHECK(parses(startup));
  CHECK(startup.find("\"first_detour_ms\":14300.250") != std::string::npos);
  CHECK(startup.find("\"ipc_start_ms\":null") != std::string::npos);
  CHECK(startup.find("\"menu_timed_out\":false") != std::string::npos);

  hook_totals totals;
  totals.created = 2;
  totals.enables = 2;
  totals.create_ms = 0.5;
  totals.enable_ms = 86.25;
  totals.max_enable_ms = 60.125;
  totals.timeline = {{14300.25, 120.5, 26125.0}, {14400.0, 80.0, 60125.0}};
  const std::string hooks = hooks_json(totals);
  REQUIRE(parses(hooks));
  CHECK(hooks.find("\"timeline\":[[14300.25,120.5,26125.0],[14400.00,80.0,"
                   "60125.0]]") != std::string::npos);
  totals.timeline.clear();
  CHECK(hooks_json(totals).find("\"timeline\":[]") != std::string::npos);
}
