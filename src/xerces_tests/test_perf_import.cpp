// Reading benchmark inputs (perf plan H1): the probe's and PresentMon's frame
// CSVs, console load lines and JSON log events, on small snippets written
// for these tests (the column names and line forms are the real ones; the
// numbers are made up).

#include "harness.hpp"

#include <cmath>
#include <string>
#include <vector>

#include <xerces_core/perf_import.hpp>

using namespace xerces_core::perf;

namespace {
bool near(double actual, double expected, double tolerance = 1e-9) {
  return std::fabs(actual - expected) <= tolerance;
}

#define CHECK_NEAR(actual, expected, tolerance)                                \
  ::xerces_tests::check_equal(near((actual), (expected), (tolerance)), true,   \
                              #actual " ~= " #expected, __FILE__, __LINE__,    \
                              false)
} // namespace

TEST_CASE("perf import: the probe's frames CSV") {
  const std::string csv = "i,t_ms,dt_ms,present_ms,overlay_us,cpu,flags\r\n"
                          "0,1000.0,,0.20,12,3,0\r\n"
                          "1,1004.0,4.0,0.25,10,3,0\r\n"
                          "2,1008.5,4.5,0.30,11,5,1\r\n"
                          "3,1012.5,4.0,NA,9,5,0\r\n"
                          "4,1030.5,18.0,0.40,30,3,0\r\n";
  const auto result = parse_frames_csv(csv);
  REQUIRE(result.series.has_value());
  const frame_series &s = *result.series;
  CHECK(s.format == frames_format::probe);
  CHECK_EQ(s.dt_column, std::string("dt_ms"));
  CHECK_EQ(s.time_column, std::string("t_ms"));
  CHECK_EQ(s.rows, 5u);
  CHECK_EQ(s.skipped, 1u); // the first frame has no interval yet
  REQUIRE_EQ(s.dt_ms.size(), 4u);
  CHECK_NEAR(s.dt_ms[1], 4.5, 0);
  CHECK_NEAR(s.dt_ms[3], 18.0, 0);
  CHECK_NEAR(s.t_ms[0], 0.0, 0);
  CHECK_NEAR(s.t_ms[3], 26.5, 1e-9);
  REQUIRE_EQ(s.present_ms.size(), 4u);
  CHECK(std::isnan(s.present_ms[2]));
  CHECK_NEAR(s.overlay_us[3], 30.0, 0);
  CHECK_EQ(s.cpus.at(3), 2u);
  CHECK_EQ(s.cpus.at(5), 2u);
  CHECK_EQ(s.flagged, 1u);

  CHECK_EQ(s.warmup_rows, 0u);

  // A warm-up of 8 ms keeps the frames from t = 8 ms on, and the counts
  // cover only them: rows 3 and 4 (CPUs 5 and 3, no flags).
  const frame_series kept = drop_warmup(s, 0.008);
  REQUIRE_EQ(kept.dt_ms.size(), 2u);
  CHECK_NEAR(kept.dt_ms[0], 4.0, 0);
  CHECK_NEAR(kept.dt_ms[1], 18.0, 0);
  CHECK_EQ(kept.present_ms.size(), 2u);
  CHECK_EQ(kept.rows, 2u);
  CHECK_EQ(kept.skipped, 0u);
  CHECK_EQ(kept.warmup_rows, 3u);
  CHECK_EQ(kept.warmup_frames, 2u);
  CHECK_EQ(kept.cpus.size(), 2u);
  CHECK_EQ(kept.cpus.at(3), 1u);
  CHECK_EQ(kept.cpus.at(5), 1u);
  CHECK_EQ(kept.flagged, 0u);
  REQUIRE_EQ(kept.row.size(), 2u);
  CHECK_EQ(kept.row[0], 0u);
  // Again on what is left: 10 ms after its first frame (8.5) is 18.5, so
  // only the last frame stays.
  const frame_series again = drop_warmup(kept, 0.010);
  REQUIRE_EQ(again.dt_ms.size(), 1u);
  CHECK_EQ(again.rows, 1u);
  CHECK_EQ(again.warmup_rows, 4u);
  CHECK_EQ(again.warmup_frames, 3u);
  CHECK_EQ(drop_warmup(s, 0).dt_ms.size(), 4u);
  CHECK_EQ(drop_warmup(s, 0).cpus.at(3), 2u);

  // A warm-up longer than the run keeps nothing.
  const frame_series none = drop_warmup(s, 1);
  CHECK(none.dt_ms.empty());
  CHECK_EQ(none.rows, 0u);
  CHECK(none.cpus.empty());
}

TEST_CASE("perf import: rows without a frame time and the warm-up") {
  const std::string csv = "i,t_ms,dt_ms,cpu,flags\n"
                          "0,0,4,1,1\n"
                          "1,4,,1,1\n"
                          "2,8,4,1,1\n"
                          "3,12,,2,0\n"
                          "4,16,4,2,0\n";
  const auto result = parse_frames_csv(csv);
  REQUIRE(result.series.has_value());
  CHECK_EQ(result.series->rows, 5u);
  CHECK_EQ(result.series->skipped, 2u);
  CHECK_EQ(result.series->flagged, 2u); // rows 1 and 3 are not frames
  // From t = 6 ms the first frame kept is row 2: rows 0 and 1 (one of them
  // without a frame time) go with the warm-up, row 3 stays.
  const frame_series kept = drop_warmup(*result.series, 0.006);
  REQUIRE_EQ(kept.dt_ms.size(), 2u);
  CHECK_EQ(kept.rows, 3u);
  CHECK_EQ(kept.skipped, 1u);
  CHECK_EQ(kept.warmup_rows, 2u);
  CHECK_EQ(kept.warmup_frames, 1u);
  CHECK_EQ(kept.flagged, 1u);
  CHECK_EQ(kept.cpus.at(1), 1u);
  CHECK_EQ(kept.cpus.at(2), 1u);
}

TEST_CASE("perf import: PresentMon 2.3+ CSV, the game's rows only") {
  const std::string csv =
      "Application,ProcessID,SwapChainAddress,PresentRuntime,SyncInterval,"
      "PresentFlags,AllowsTearing,PresentMode,FrameType,CPUStartTime,"
      "MsBetweenPresents,MsUntilDisplayed,MsBetweenAppStart,MsCPUBusy,"
      "MsCPUWait,MsGPUBusy,MsGPUWait\n"
      "boiii.exe,1234,0x00000001,DXGI,0,512,1,Hardware: Independent "
      "Flip,Application,10.000000,4.100,5.0,4.1,2.5,1.6,3.4,0.7\n"
      "Discord.exe,999,0x000000AA,DXGI,1,0,0,Composed: "
      "Flip,Application,10.001000,16.600,20.0,16.6,1.0,15.6,0.5,16.1\n"
      "boiii.exe,1234,0x00000001,DXGI,0,512,1,Hardware: Independent "
      "Flip,Application,10.004100,4.200,NA,4.2,2.6,1.6,3.5,0.7\n"
      "boiii.exe,1234,0x00000001,DXGI,0,512,1,Composed: "
      "Flip,Application,10.008300,3.900,4.8,3.9,2.4,1.5,3.3,0.6\n";
  const auto result = parse_frames_csv(csv);
  REQUIRE(result.series.has_value());
  const frame_series &s = *result.series;
  CHECK(s.format == frames_format::presentmon);
  CHECK_EQ(s.application, std::string("boiii.exe"));
  CHECK_EQ(s.process_id, std::string("1234"));
  CHECK_EQ(s.dt_column, std::string("MsBetweenPresents"));
  CHECK_EQ(s.time_column, std::string("CPUStartTime"));
  CHECK_EQ(s.rows, 3u);
  CHECK_EQ(s.other_rows, 1u);
  REQUIRE_EQ(s.dt_ms.size(), 3u);
  CHECK_NEAR(s.dt_ms[2], 3.9, 1e-12);
  CHECK_NEAR(s.t_ms[1], 4.1, 1e-6);
  CHECK_NEAR(s.t_ms[2], 8.3, 1e-6);
  REQUIRE_EQ(s.cpu_busy_ms.size(), 3u);
  CHECK_NEAR(s.cpu_busy_ms[1], 2.6, 1e-12);
  CHECK_NEAR(s.gpu_busy_ms[2], 3.3, 1e-12);
  CHECK_EQ(s.not_displayed, 1u);
  CHECK_EQ(s.present_modes.at("Hardware: Independent Flip"), 2u);
  CHECK_EQ(s.present_modes.at("Composed: Flip"), 1u);

  // After a 5 ms warm-up only the last frame is left: its present mode, and
  // the frame never displayed went with the warm-up. Other processes' rows
  // stay a count of the whole capture.
  const frame_series kept = drop_warmup(s, 0.005);
  REQUIRE_EQ(kept.dt_ms.size(), 1u);
  CHECK_EQ(kept.rows, 1u);
  CHECK_EQ(kept.warmup_rows, 2u);
  CHECK_EQ(kept.not_displayed, 0u);
  CHECK_EQ(kept.present_modes.size(), 1u);
  CHECK_EQ(kept.present_modes.at("Composed: Flip"), 1u);
  CHECK_EQ(kept.other_rows, 1u);
  REQUIRE_EQ(kept.cpu_busy_ms.size(), 1u);
  CHECK_NEAR(kept.cpu_busy_ms[0], 2.4, 1e-12);
}

TEST_CASE("perf import: PresentMon 2.0 and 1.x column names") {
  const std::string v20 =
      "Application,ProcessID,SwapChainAddress,PresentRuntime,SyncInterval,"
      "PresentFlags,AllowsTearing,PresentMode,CPUStartTime,FrameTime,CPUBusy,"
      "CPUWait,GPULatency,GPUTime,GPUBusy,GPUWait,VideoBusy,DisplayLatency,"
      "DisplayedTime\n"
      "boiii.exe,1234,0x1,DXGI,0,512,1,Hardware: Independent "
      "Flip,0.5,4.0,2.0,2.0,0.5,3.0,3.0,1.0,0,6.0,4.0\n"
      "boiii.exe,1234,0x1,DXGI,0,512,1,Hardware: Independent "
      "Flip,0.504,5.0,2.5,2.5,0.5,4.0,4.0,1.0,0,7.0,5.0\n";
  const auto two = parse_frames_csv(v20);
  REQUIRE(two.series.has_value());
  CHECK_EQ(two.series->dt_column, std::string("FrameTime"));
  REQUIRE_EQ(two.series->dt_ms.size(), 2u);
  CHECK_NEAR(two.series->t_ms[1], 4.0, 1e-6);
  CHECK_NEAR(two.series->cpu_busy_ms[1], 2.5, 0);
  CHECK_NEAR(two.series->gpu_busy_ms[0], 3.0, 0);
  CHECK_EQ(two.series->not_displayed, 0u);

  const std::string v1 =
      "Application,ProcessID,SwapChainAddress,Runtime,SyncInterval,"
      "PresentFlags,Dropped,TimeInSeconds,msInPresentAPI,msBetweenPresents,"
      "AllowsTearing,PresentMode,msUntilRenderComplete,msUntilDisplayed,"
      "msBetweenDisplayChange\n"
      "boiii.exe,1234,0x1,DXGI,0,512,0,1.000,0.1,4.000,1,Hardware: Independent "
      "Flip,3.0,5.0,4.0\n"
      "boiii.exe,1234,0x1,DXGI,0,512,1,1.004,0.1,4.000,1,Hardware: Independent "
      "Flip,3.0,5.0,4.0\n";
  const auto one = parse_frames_csv(v1);
  REQUIRE(one.series.has_value());
  CHECK_EQ(one.series->dt_column, std::string("msBetweenPresents"));
  CHECK_EQ(one.series->time_column, std::string("TimeInSeconds"));
  CHECK_EQ(one.series->not_displayed, 1u); // Dropped = 1
  CHECK(one.series->cpu_busy_ms.empty());

  // Neither format, or nothing at all.
  CHECK(!parse_frames_csv("a,b,c\n1,2,3\n").series.has_value());
  CHECK(!parse_frames_csv("").series.has_value());
  CHECK(!parse_frames_csv("i,t_ms,dt_ms\n0,0,\n").series.has_value());
}

TEST_CASE("perf import: console load lines, sections and zone groups") {
  const std::string console =
      "Command line: -launch -nointro -xerces-plan abc +set fs_game "
      "1944930083 +set fs_extramods 3171632315,3656779213\r\n"
      "----- FS_Startup -----\r\n"
      "FastFileLoad: Setting en_core_pre_gfx to XZONE_LOADED in 62ms.\r\n"
      "FastFileLoad: Setting core_mod to XZONE_LOADED in 300ms.\r\n"
      "FastFileLoad: Setting stack_3171632315_core_mod to XZONE_LOADED in "
      "120ms.\r\n"
      "[ Workshop ] Supplemented 8 usermaps from Steam workshop\r\n"
      "[ Workshop ] Supplemented 9 mods from Steam workshop\r\n"
      "FastFileLoad: Setting core_frontend to XZONE_LOADED in 2347ms.\r\n"
      "]map zm_log_overunder\r\n"
      "ModLoad start: usermaps\r\n"
      "----- FS_Startup -----\r\n"
      "FastFileLoad: Setting en_core_frontend to XZONE_LOADED in 400ms.\r\n"
      "FastFileLoad: Setting core_frontend_patch to XZONE_LOADED in 519ms.\r\n"
      "ModLoad done: usermaps\r\n"
      "FastFileLoad: Setting zm_levelcommon to XZONE_LOADED in 426ms.\r\n"
      "FastFileLoad: Setting en_zm_log_overunder to XZONE_LOADED in 213ms.\r\n"
      "FastFileLoad: Setting zm_log_overunder to XZONE_LOADED in 812ms.\r\n"
      "FastFileLoad: Setting en_stack_3656779213_zm_mod to XZONE_LOADED in 90 "
      "ms.\r\n"
      "FastFileLoad: Setting zm_mod to XZONE_LOADED in 150ms.\r\n"
      "Loaded sounds for 'zm_log_overunder' 0ms\r\n"
      "FastFileLoad: Setting broken to XZONE_LOADING.\r\n"
      "]disconnect\r\n"
      "FastFileLoad: Setting core_frontend to XZONE_LOADED in 2374ms.\r\n";
  const console_log log = parse_console(console);
  CHECK(log.from_start);
  CHECK_EQ(log.lines, 23u);
  CHECK_EQ(log.fs_game, std::string("1944930083"));
  REQUIRE_EQ(log.fs_extramods.size(), 2u);
  CHECK_EQ(log.fs_extramods[1], std::string("3656779213"));

  const auto sections = console_sections(log);
  REQUIRE_EQ(sections.size(), 3u);
  const console_section &start = sections[0];
  CHECK_EQ(start.kind, std::string("startup"));
  CHECK_EQ(start.last_line, 8u);
  CHECK_EQ(start.zones.size(), 4u);
  CHECK_NEAR(start.zones_ms, 2829.0, 0);
  CHECK_NEAR(start.ms_by_group.at(zone_group::main_mod), 300.0, 0);
  CHECK_NEAR(start.ms_by_group.at(zone_group::extra_mod), 120.0, 0);
  CHECK_NEAR(start.ms_by_group.at(zone_group::frontend), 2347.0, 0);
  CHECK_NEAR(start.ms_by_group.at(zone_group::stock), 62.0, 0);
  CHECK_EQ(start.fs_startups, 1u);
  REQUIRE_EQ(start.supplemented.size(), 2u);
  CHECK_EQ(start.supplemented[0].first, std::string("usermaps"));
  CHECK_EQ(start.supplemented[1].second, 9u);

  const console_section &map = sections[1];
  CHECK_EQ(map.kind, std::string("map"));
  CHECK_EQ(map.map, std::string("zm_log_overunder"));
  CHECK_EQ(map.first_line, 9u);
  CHECK_EQ(map.last_line, 21u);
  CHECK_EQ(map.zones.size(), 7u);
  CHECK_NEAR(map.zones_ms, 2610.0, 0);
  CHECK_NEAR(map.ms_by_group.at(zone_group::map), 1025.0, 0);
  CHECK_EQ(map.zones_by_group.at(zone_group::map), 2u);
  CHECK_NEAR(map.ms_by_group.at(zone_group::frontend), 919.0, 0);
  CHECK_NEAR(map.ms_by_group.at(zone_group::extra_mod), 90.0, 0);
  CHECK_NEAR(map.ms_by_group.at(zone_group::main_mod), 150.0, 0);
  CHECK_NEAR(map.ms_by_group.at(zone_group::stock), 426.0, 0);
  REQUIRE_EQ(map.mod_loads.size(), 1u);
  CHECK_EQ(map.mod_loads[0].name, std::string("usermaps"));
  CHECK_EQ(map.mod_loads[0].zones, 2u);
  CHECK_NEAR(map.mod_loads[0].zones_ms, 919.0, 0);
  CHECK(map.mod_loads[0].done);

  // The map's own zone came last: the whole section is the load.
  const map_load_zones load = map_load_part(map);
  CHECK(load.map_zone_loaded);
  CHECK_EQ(load.zones, 5u); // up to zm_log_overunder; the mod zones follow
  CHECK_NEAR(load.zones_ms, 2370.0, 0);
  CHECK_EQ(load.after_zones, 2u);
  CHECK_NEAR(load.after_ms, 240.0, 0);
  CHECK_NEAR(load.ms_by_group.at(zone_group::frontend), 919.0, 0);

  const console_section &after = sections[2];
  CHECK_EQ(after.kind, std::string("command"));
  CHECK_EQ(after.command, std::string("disconnect"));
  CHECK_EQ(after.last_line, 23u);
  CHECK_NEAR(after.ms_by_group.at(zone_group::frontend), 2374.0, 0);

  // A drop back to the menu after the map loaded is not part of the load;
  // a map that never loaded is all load.
  const console_log dropped = parse_console(
      "]map zm_zod\n"
      "FastFileLoad: Setting zm_common to XZONE_LOADED in 700ms.\n"
      "FastFileLoad: Setting zm_zod to XZONE_LOADED in 900ms.\n"
      "FastFileLoad: Setting core_frontend to XZONE_LOADED in 2300ms.\n");
  const map_load_zones zod = map_load_part(console_sections(dropped).at(1));
  CHECK_NEAR(zod.zones_ms, 1600.0, 0);
  CHECK_NEAR(zod.after_ms, 2300.0, 0);
  CHECK(!zod.ms_by_group.contains(zone_group::frontend));
  const console_log crashed = parse_console(
      "]map zm_zod\n"
      "FastFileLoad: Setting core_frontend to XZONE_LOADED in 2300ms.\n");
  const map_load_zones none = map_load_part(console_sections(crashed).at(1));
  CHECK(!none.map_zone_loaded);
  CHECK_NEAR(none.zones_ms, 2300.0, 0);
  CHECK_EQ(none.after_zones, 0u);

  // A console copied after its start was lost.
  const console_log late =
      parse_console("FastFileLoad: Setting core_ui to XZONE_LOADED in 12ms.\n");
  CHECK(!late.from_start);
  CHECK(late.fs_extramods.empty());
  REQUIRE_EQ(late.events.size(), 1u);
  CHECK_NEAR(late.events[0].ms, 12.0, 0);
}

TEST_CASE("perf import: zone groups") {
  const std::vector<std::string> extras{"3171632315"};
  CHECK(group_of_zone("core_frontend", "", extras) == zone_group::frontend);
  CHECK(group_of_zone("en_core_frontend_patch", "", extras) ==
        zone_group::frontend);
  CHECK(group_of_zone("zm_zod", "zm_zod", extras) == zone_group::map);
  CHECK(group_of_zone("en_zm_zod", "zm_zod", extras) == zone_group::map);
  CHECK(group_of_zone("zm_zod", "", extras) == zone_group::stock);
  CHECK(group_of_zone("core_ui", "", extras) == zone_group::stock);
  CHECK(group_of_zone("zm_mod", "", extras) == zone_group::main_mod);
  CHECK(group_of_zone("stack_3171632315_zm_mod", "", extras) ==
        zone_group::extra_mod);
  CHECK_EQ(std::string(to_string(zone_group::extra_mod)),
           std::string("extra_mod"));
}

TEST_CASE("perf import: log timestamps") {
  CHECK_EQ(parse_timestamp_ms("2026-11-05T18:18:31.655Z").value_or(-1),
           1793902711655ll);
  CHECK_EQ(parse_timestamp_ms("1970-01-01T00:00:00Z").value_or(-1), 0ll);
  CHECK_EQ(parse_timestamp_ms("2024-02-29T00:00:00Z").value_or(-1),
           1709164800000ll);
  CHECK_EQ(parse_timestamp_ms("2026-11-05T19:18:31.655+01:00").value_or(-1),
           1793902711655ll);
  CHECK_EQ(parse_timestamp_ms("2026-11-05T18:18:31Z").value_or(-1),
           1793902711000ll);
  CHECK_EQ(parse_timestamp_ms("2026-11-05 18:18:31.6554").value_or(-1),
           1793902711655ll);
  CHECK(!parse_timestamp_ms("2026-13-05T00:00:00Z").has_value());
  CHECK(!parse_timestamp_ms("2026-11-05T18:18:31.Z").has_value());
  CHECK(!parse_timestamp_ms("2026-11-05T18:18:31Zjunk").has_value());
  CHECK(!parse_timestamp_ms("yesterday").has_value());
}

TEST_CASE("perf import: JSON log events and startup marks") {
  const std::string jsonl =
      "\xEF\xBB\xBF"
      R"({"component":"launcher","data":{"ms":14.7},"event":"launcher.startup.step","level":"info","msg":"window created at 14.7 ms","role":"launcher","session":"46220","ts":"2026-11-05T18:18:27.647Z"})"
      "\n"
      "not json\n"
      R"({"component":"launcher","data":{"pid":17780,"plan_id":"sha256:7f"},"event":"launcher.launch.started","level":"info","msg":"-launch","role":"launcher","session":"46220","ts":"2026-11-05T18:18:31.655Z"})"
      "\r\n"
      R"({"no_event":1})"
      "\n"
      R"({"component":"launcher","data":{"dry_launch":true,"home":"x"},"event":"launcher.start","level":"info","msg":"Xerces","role":"launcher","session":"1","ts":"bad"})";
  const auto events = parse_log_events(jsonl);
  REQUIRE_EQ(events.size(), 3u);
  CHECK_EQ(events[0].event, std::string("launcher.startup.step"));
  CHECK_NEAR(events[0].number("ms").value_or(-1), 14.7, 1e-12);
  CHECK_EQ(events[0].ts_ms.value_or(-1), 1793902707647ll);
  CHECK_EQ(events[0].session, std::string("46220"));
  CHECK_NEAR(events[1].number("pid").value_or(-1), 17780.0, 0);
  CHECK_EQ(events[1].text("plan_id"), std::string("sha256:7f"));
  CHECK_EQ(events[2].text("dry_launch"), std::string("true"));
  CHECK(!events[2].ts_ms.has_value());
  CHECK(!events[2].number("missing").has_value());
  CHECK(events[2].text("missing").empty());

  CHECK_EQ(startup_mark("window created at 14.7 ms"),
           std::string("window created"));
  CHECK_EQ(startup_mark("renderer shaders (worker) at 201.0 ms"),
           std::string("renderer shaders (worker)"));
  CHECK_EQ(startup_mark("no time here"), std::string("no time here"));
  CHECK_EQ(startup_mark("looks at nothing ms"),
           std::string("looks at nothing ms"));
}
