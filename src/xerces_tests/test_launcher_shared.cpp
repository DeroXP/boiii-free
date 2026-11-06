// The launcher code xerces_cli shares with Xerces.exe (docs/DESIGN.md 5.4,
// 5.6, 8.3, 15.1, 15.2): a profile turned into a plan request, the launch on
// a fake game folder (dry, and refused), applying a pack to a profile, the
// child's environment, the per-home mutex, the log tail and the game's
// signature report.

#include "harness.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <string>
#include <vector>

#include <xerces_core/backup.hpp>
#include <xerces_core/journal.hpp>
#include <xerces_core/launch.hpp>
#include <xerces_core/launcher_home.hpp>
#include <xerces_core/launcher_launch.hpp>
#include <xerces_core/launcher_state.hpp>
#include <xerces_core/log_tail.hpp>
#include <xerces_core/mods_info.hpp>
#include <xerces_core/pack_apply.hpp>

using namespace xerces_core;
namespace fs = std::filesystem;

namespace {
fs::path fresh(const std::string &name) {
  const fs::path dir = fs::temp_directory_path() / ("xerces_shared_" + name);
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  return dir;
}

void write(const fs::path &file, const std::string &text) {
  fs::create_directories(file.parent_path());
  std::ofstream(file, std::ios::binary) << text;
}

// A file the pre-flight reads as a Windows executable with this checksum: the
// DOS header, "PE\0\0" at 0x40 and the optional header's CheckSum.
std::string fake_pe(std::uint32_t checksum) {
  std::string out(0x40 + 24 + 96, '\0');
  const auto put = [&](std::size_t at, std::uint32_t value) {
    std::memcpy(out.data() + at, &value, 4);
  };
  out[0] = 'M';
  out[1] = 'Z';
  put(0x3C, 0x40);
  put(0x40, 0x00004550);
  put(0x40 + 24 + 64, checksum);
  return out;
}

constexpr std::uint32_t fake_build_checksum = 0x7E570001;

// A home and a fake game: <root>\steamapps\common\game with a BlackOps3.exe
// and the fixture mods in its mods folder. Unsupported: a two-byte exe the
// pre-flight cannot read. Supported: a fake executable that root\compat\
// builds.json lists as a supported build. The client is never there.
struct fake_setup {
  fs::path root;
  launcher_paths paths;
  launcher_config config;
  game_state game;
  catalog_state catalog;

  explicit fake_setup(const std::string &name, bool supported = false)
      : root(fresh(name)) {
    const fs::path game_dir = root / "steamapps" / "common" / "game";
    write(game_dir / "BlackOps3.exe",
          supported ? fake_pe(fake_build_checksum) : std::string("MZ"));
    if (supported) {
      write(root / "compat" / "builds.json",
            R"({"schema":"xerces.builds/1","builds":[{"name":"Fake test )"
            R"(build","exe":"BlackOps3.exe","checksum":"0x7E570001",)"
            R"("status":"supported"}]})");
    }
    fs::copy(xerces_tests::fixtures_dir() / "mods", game_dir / "mods",
             fs::copy_options::recursive);
    paths.home = root / "home";
    paths.config = paths.home / "config";
    paths.plans = paths.config / "plans";
    paths.cache = paths.home / "cache";
    paths.logs = paths.home / "logs";
    paths.crash = paths.home / "crash";
    paths.compat = root / "compat";
    paths.client = root / "bin" / "boiii.exe"; // missing
    paths.program = root / "bin" / "xerces_cli.exe";
    config.game_dir = path_utf8(game_dir);
    game = detect_game_state(config.game_dir, paths.compat);
    catalog = scan_catalog(game, paths.cache / "hashes.txt");
  }
  ~fake_setup() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }

  launch_inputs inputs(bool dry) const {
    launch_inputs in;
    in.paths = &paths;
    in.config = &config;
    in.game = &game;
    in.catalog = &catalog.items;
    in.mode = "zm";
    in.dry_launch = dry;
    return in;
  }
};

struct recorded {
  std::vector<std::string> events;
  event_log sink() {
    return [this](log_level, std::string_view event, std::string_view,
                  std::string_view) { events.emplace_back(event); };
  }
  bool has(const std::string &event) const {
    return std::ranges::find(events, event) != events.end();
  }
};

pack sample_pack() {
  pack p;
  p.id = "7b1e2c4a-1f0d-4f5e-9a51-3c2f8d0e9b11";
  p.name = "Weekend Zombies";
  p.mode = "zm";
  pack_mod a;
  a.id = "fixture.alpha";
  a.ver = *version::parse("1.0.0");
  a.settings = {{"lives", "3"}};
  pack_mod b;
  b.id = "fixture.beta";
  b.ver = *version::parse("1.0.0");
  p.mods = {a, b};
  p.order = {"fixture.beta", "fixture.alpha"};
  p.map = pack_map{"ws.3735179543", ""};
  p.decisions.allow_conflicts = {{"fixture.alpha", "fixture.beta"}};
  return p;
}
} // namespace

TEST_CASE("shared launcher: a profile becomes the plan request") {
  launcher_config config;
  mode_profile &zm = profile_for(config, "zm");
  zm.mods = {"fixture.alpha", "fixture.beta"};
  zm.map = "ws.3735179543";
  zm.pins = {{"fixture.alpha", "1.0.0"}};
  zm.allowed_conflicts = {{"fixture.alpha", "fixture.beta"}};
  zm.settings = {{"fixture.alpha", {{"lives", "3"}}}};
  config.saved_positions = {{"fixture.beta", 4}};

  const plan_request request =
      profile_request(config, "zm", std::uint64_t(24784313));
  CHECK_EQ(request.mode, std::string("zm"));
  CHECK(request.mods == zm.mods);
  CHECK(request.map == zm.map);
  CHECK(request.pins == zm.pins);
  CHECK(request.allowed_conflicts == zm.allowed_conflicts);
  CHECK(request.settings == zm.settings);
  CHECK(request.saved_positions == config.saved_positions);
  CHECK_EQ(request.game_build, std::string("24784313"));

  // A mode without a profile asks for nothing; no Steam build, no game_build.
  const plan_request mp = profile_request(config, "mp", std::nullopt);
  CHECK(mp.mods.empty());
  CHECK(mp.pins.empty());
  CHECK(mp.game_build.empty());
}

TEST_CASE("shared launcher: the profile resolves against the catalog") {
  fake_setup setup("resolve");
  REQUIRE(setup.game.dir.has_value());
  REQUIRE_EQ(setup.catalog.items.size(), std::size_t(3));
  CHECK(setup.catalog.scanned);
  CHECK_EQ(setup.catalog.sources.front(), std::string("mods"));
  profile_for(setup.config, "zm").mods = {"fixture.alpha"};
  const plan p = resolve_profile(setup.catalog.items, setup.config, "zm",
                                 setup.game.steam_build);
  REQUIRE(p.ok());
  REQUIRE_EQ(p.stack.size(), std::size_t(2)); // with its dependency
  CHECK_EQ(p.stack[0].id, std::string("fixture.alpha"));
  CHECK_EQ(p.stack[1].id, std::string("fixture.probe"));
  // A culprit test launch replaces the list.
  const std::vector<std::string> only = {"fixture.beta"};
  const plan test = resolve_profile(setup.catalog.items, setup.config, "zm",
                                    setup.game.steam_build, &only);
  REQUIRE(!test.stack.empty());
  CHECK_EQ(test.stack[0].id, std::string("fixture.beta"));
  // The hash cache was written for the next scan.
  CHECK(fs::exists(setup.paths.cache / "hashes.txt"));
}

TEST_CASE("shared launcher: a dry launch writes the plan, no journal") {
  fake_setup setup("dry", true);
  profile_for(setup.config, "zm").mods = {"fixture.alpha"};
  REQUIRE(setup.game.can_launch()); // the fake build passes the pre-flight
  recorded log;
  bool asked_running = false;
  const launch_outcome out = launch_game(
      setup.inputs(true),
      [&] {
        asked_running = true;
        return false;
      },
      log.sink());
  CHECK(out.dry_run);
  CHECK(!out.started);
  CHECK(asked_running);
  CHECK(out.refusal == launch_refusal::none);
  CHECK(out.command_line.starts_with("-launch -nointro -xerces-plan "));
  CHECK(out.command_line.ends_with(
      "+set fs_game fixture.alpha +set fs_extramods fixture.probe"));
  // It says what a real launch would refuse.
  CHECK(out.message.find("a real launch would stop") != std::string::npos);
  CHECK(out.message.find("boiii.exe is missing") != std::string::npos);
  REQUIRE(!out.plan_file.empty());
  CHECK(fs::exists(out.plan_file));
  CHECK_EQ(out.plan_file.filename().string(),
           plan_file_id(out.plan_id) + ".json");
  CHECK(!fs::exists(setup.paths.journal_file()));
  CHECK(log.has("launch.dry"));

  // A real launch on the same setup stops at the missing client, before the
  // plan file or the journal.
  std::error_code ec;
  fs::remove(out.plan_file, ec);
  recorded real;
  const launch_outcome refused =
      launch_game(setup.inputs(false), [] { return false; }, real.sink());
  CHECK(!refused.started);
  CHECK(refused.refusal == launch_refusal::client);
  CHECK(refused.diags.has("launch.client"));
  CHECK(refused.plan_file.empty());
  CHECK(!fs::exists(out.plan_file));
  CHECK(!fs::exists(setup.paths.journal_file()));
}

TEST_CASE("shared launcher: refusals") {
  fake_setup setup("refused");
  profile_for(setup.config, "zm").mods = {"fixture.alpha"};
  REQUIRE(!setup.game.can_launch()); // the two-byte exe is no game build
  for (const bool dry : {false, true}) {
    // The unsupported game stops a real launch and a dry one alike, as it
    // did in Xerces.exe before the launch was shared: no plan file, no
    // journal, no launch.dry.
    recorded log;
    const launch_outcome out =
        launch_game(setup.inputs(dry), [] { return false; }, log.sink());
    CHECK(!out.started);
    CHECK(!out.dry_run);
    CHECK(out.refusal == launch_refusal::game);
    CHECK(out.diags.has("launch.game"));
    CHECK(out.message.find("could not be read") != std::string::npos);
    CHECK(log.has("launch.refused"));
    CHECK(!log.has("launch.dry"));
    CHECK(!fs::exists(setup.paths.journal_file()));
    CHECK(out.plan_file.empty());
    CHECK(!fs::exists(setup.paths.plans));
  }
  {
    // A running game stops even a dry launch.
    fake_setup supported("refused_running", true);
    profile_for(supported.config, "zm").mods = {"fixture.alpha"};
    recorded log;
    const launch_outcome out =
        launch_game(supported.inputs(true), [] { return true; }, log.sink());
    CHECK(!out.dry_run);
    CHECK(out.refusal == launch_refusal::running);
    CHECK(log.has("launch.refused"));
  }
  {
    // A mod that is not installed is a plan problem.
    profile_for(setup.config, "zm").mods = {"ws.1"};
    recorded log;
    const launch_outcome out =
        launch_game(setup.inputs(true), [] { return false; }, log.sink());
    CHECK(out.refusal == launch_refusal::plan);
    CHECK(out.diags.has_errors());
    CHECK(out.command_line.empty());
  }
  {
    // No mods enabled: launch vanilla instead.
    profile_for(setup.config, "zm").mods.clear();
    const launch_outcome out = preview_launch(setup.inputs(true));
    CHECK(out.refusal == launch_refusal::plan);
    CHECK(out.diags.has("launch.plan.empty"));
  }
  {
    // A setting the command line cannot carry.
    setup.config.dvars = {{"name", "a;quit"}};
    launch_inputs in = setup.inputs(true);
    in.modded = false;
    const launch_outcome out = preview_launch(in);
    CHECK(out.refusal == launch_refusal::command);
  }
}

TEST_CASE("shared launcher: vanilla and safe mode need no plan") {
  fake_setup setup("vanilla", true);
  launch_inputs in = setup.inputs(true);
  in.modded = false;
  launch_outcome out = launch_game(in, [] { return false; }, {});
  CHECK(out.dry_run);
  CHECK_EQ(out.command_line, std::string("-launch -nointro"));
  CHECK(out.plan_file.empty());
  in.modded = true;
  in.mods_off = true;
  out = launch_game(in, [] { return false; }, {});
  CHECK(out.dry_run);
  CHECK_EQ(out.command_line, std::string("-launch -nointro"));
}

TEST_CASE("shared launcher: safe mode leaves the larger zone memory off") {
  fake_setup setup("db_pool", true);
  setup.config.extra_flags = {"-windowed", "-xerces-db-pool"};
  launch_inputs in = setup.inputs(true);
  in.modded = false;
  launch_outcome out = launch_game(in, [] { return false; }, {});
  CHECK(out.dry_run);
  CHECK_EQ(out.command_line,
           std::string("-launch -nointro -windowed -xerces-db-pool"));
  CHECK(out.db_pool);
  in.modded = true;
  in.mods_off = true;
  out = launch_game(in, [] { return false; }, {});
  CHECK(out.dry_run);
  CHECK_EQ(out.command_line, std::string("-launch -nointro -windowed"));
  CHECK(!out.db_pool);
  // Only for that launch: the setting itself stays on.
  CHECK((setup.config.extra_flags ==
         std::vector<std::string>{"-windowed", "-xerces-db-pool"}));
}

TEST_CASE("shared launcher: culprit tests leave the larger zone memory off") {
  // The crashed launch had the flag. Safe mode and every culprit-search test
  // (services::launch_culprit_test: part of the crashed stack, crash_followup)
  // run without it, so a crash the setting causes never narrows the mods.
  fake_setup setup("db_pool_culprit", true);
  setup.config.extra_flags = {"-windowed", "-xerces-db-pool"};
  profile_for(setup.config, "zm").mods = {"fixture.alpha", "fixture.beta"};
  launch_inputs in = setup.inputs(true);
  launch_outcome out = launch_game(in, [] { return false; }, {});
  REQUIRE(out.dry_run);
  CHECK(out.db_pool); // what the journal records
  CHECK(out.command_line.starts_with(
      "-launch -nointro -windowed -xerces-db-pool -xerces-plan "));

  const std::vector<std::string> half = {"fixture.alpha"};
  in.mods = &half;
  in.crash_followup = true;
  out = launch_game(in, [] { return false; }, {});
  REQUIRE(out.dry_run);
  CHECK(!out.db_pool);
  CHECK(
      out.command_line.starts_with("-launch -nointro -windowed -xerces-plan "));
  CHECK(out.command_line.find("xerces-db-pool") == std::string::npos);
  CHECK(out.command_line.ends_with(
      "+set fs_game fixture.alpha +set fs_extramods fixture.probe"));

  // The first test of a crash with the flag: the whole stack, without it.
  const std::vector<std::string> whole = {"fixture.alpha", "fixture.beta"};
  in.mods = &whole;
  out = launch_game(in, [] { return false; }, {});
  REQUIRE(out.dry_run);
  CHECK(!out.db_pool);
  CHECK(out.command_line.find("xerces-db-pool") == std::string::npos);
  CHECK(out.command_line.find("fixture.beta") != std::string::npos);

  // Safe mode runs on the same memory as the tests.
  in.mods = nullptr;
  in.crash_followup = false;
  in.mods_off = true;
  out = launch_game(in, [] { return false; }, {});
  REQUIRE(out.dry_run);
  CHECK(!out.db_pool);
  CHECK_EQ(out.command_line, std::string("-launch -nointro -windowed"));

  // The setting itself stays on until the player turns it off.
  CHECK((setup.config.extra_flags ==
         std::vector<std::string>{"-windowed", "-xerces-db-pool"}));
}

TEST_CASE("shared launcher: launches after a crash leave the precise "
          "limiter off") {
  fake_setup setup("precise_limiter", true);
  setup.config.extra_flags = {"-windowed", "-xerces-precise-limiter"};
  profile_for(setup.config, "zm").mods = {"fixture.alpha", "fixture.beta"};
  launch_inputs in = setup.inputs(true);
  in.modded = false;
  launch_outcome out = launch_game(in, [] { return false; }, {});
  REQUIRE(out.dry_run);
  CHECK_EQ(out.command_line,
           std::string("-launch -nointro -windowed -xerces-precise-limiter"));
  CHECK(out.precise_limiter); // what the journal records
  CHECK(!out.db_pool);

  // A culprit-search test runs on the engine's own pacing.
  in.modded = true;
  const std::vector<std::string> half = {"fixture.alpha"};
  in.mods = &half;
  in.crash_followup = true;
  out = launch_game(in, [] { return false; }, {});
  REQUIRE(out.dry_run);
  CHECK(!out.precise_limiter);
  CHECK(out.command_line.find("xerces-precise-limiter") == std::string::npos);

  // So does safe mode.
  in.mods = nullptr;
  in.crash_followup = false;
  in.mods_off = true;
  out = launch_game(in, [] { return false; }, {});
  REQUIRE(out.dry_run);
  CHECK(!out.precise_limiter);
  CHECK_EQ(out.command_line, std::string("-launch -nointro -windowed"));

  // The setting itself stays on.
  CHECK((setup.config.extra_flags ==
         std::vector<std::string>{"-windowed", "-xerces-precise-limiter"}));
}

TEST_CASE("shared launcher: crash follow-ups leave flip model off") {
  // The experimental flip-model presentation stays out of safe mode and the
  // culprit tests, like the larger zone memory, so a crash or a black screen
  // it causes is never blamed on a mod.
  fake_setup setup("flip_model", true);
  setup.config.extra_flags = {"-windowed", "-xerces-flip-model",
                              "-xerces-db-pool"};
  profile_for(setup.config, "zm").mods = {"fixture.alpha", "fixture.beta"};
  launch_inputs in = setup.inputs(true);
  in.modded = false;
  launch_outcome out = launch_game(in, [] { return false; }, {});
  REQUIRE(out.dry_run);
  CHECK_EQ(out.command_line,
           std::string("-launch -nointro -windowed -xerces-flip-model "
                       "-xerces-db-pool"));
  CHECK(out.flip_model); // what the journal records
  CHECK(out.db_pool);

  in.modded = true;
  out = launch_game(in, [] { return false; }, {});
  REQUIRE(out.dry_run);
  CHECK(out.command_line.starts_with(
      "-launch -nointro -windowed -xerces-flip-model -xerces-db-pool "
      "-xerces-plan "));
  CHECK(out.flip_model);

  const std::vector<std::string> half = {"fixture.alpha"};
  in.mods = &half;
  in.crash_followup = true;
  out = launch_game(in, [] { return false; }, {});
  REQUIRE(out.dry_run);
  CHECK(out.command_line.find("xerces-flip-model") == std::string::npos);
  CHECK(out.command_line.find("xerces-db-pool") == std::string::npos);
  CHECK(
      out.command_line.starts_with("-launch -nointro -windowed -xerces-plan "));
  CHECK(!out.flip_model);

  in.mods = nullptr;
  in.crash_followup = false;
  in.mods_off = true;
  out = launch_game(in, [] { return false; }, {});
  REQUIRE(out.dry_run);
  CHECK_EQ(out.command_line, std::string("-launch -nointro -windowed"));
  CHECK(!out.flip_model);

  // Only for those launches: the setting itself stays on.
  CHECK((setup.config.extra_flags ==
         std::vector<std::string>{"-windowed", "-xerces-flip-model",
                                  "-xerces-db-pool"}));
}

TEST_CASE("shared launcher: crash follow-ups leave every experimental "
          "setting off") {
  // All three experimental settings on: safe mode and the culprit tests
  // leave out each one (suspect_settings), the journal's flags say which the
  // launch had, and the settings themselves stay on.
  fake_setup setup("three_settings", true);
  setup.config.extra_flags = {"-windowed", "-xerces-db-pool",
                              "-xerces-precise-limiter", "-xerces-flip-model"};
  profile_for(setup.config, "zm").mods = {"fixture.alpha", "fixture.beta"};
  launch_inputs in = setup.inputs(true);
  in.modded = false;
  launch_outcome out = launch_game(in, [] { return false; }, {});
  REQUIRE(out.dry_run);
  CHECK_EQ(out.command_line,
           std::string("-launch -nointro -windowed -xerces-db-pool "
                       "-xerces-precise-limiter -xerces-flip-model"));
  CHECK(out.db_pool);
  CHECK(out.precise_limiter);
  CHECK(out.flip_model);

  in.modded = true;
  const std::vector<std::string> half = {"fixture.alpha"};
  in.mods = &half;
  in.crash_followup = true;
  out = launch_game(in, [] { return false; }, {});
  REQUIRE(out.dry_run);
  CHECK(
      out.command_line.starts_with("-launch -nointro -windowed -xerces-plan "));
  for (const auto &flag : suspect_settings()) {
    CHECK(out.command_line.find(flag) == std::string::npos);
  }
  CHECK(!out.db_pool);
  CHECK(!out.precise_limiter);
  CHECK(!out.flip_model);

  in.mods = nullptr;
  in.crash_followup = false;
  in.mods_off = true;
  out = launch_game(in, [] { return false; }, {});
  REQUIRE(out.dry_run);
  CHECK_EQ(out.command_line, std::string("-launch -nointro -windowed"));
  CHECK(!out.db_pool);
  CHECK(!out.precise_limiter);
  CHECK(!out.flip_model);

  CHECK((setup.config.extra_flags ==
         std::vector<std::string>{"-windowed", "-xerces-db-pool",
                                  "-xerces-precise-limiter",
                                  "-xerces-flip-model"}));
  CHECK((suspect_settings() ==
         std::vector<std::string>{"xerces-db-pool", "xerces-precise-limiter",
                                  "xerces-flip-model"}));
}

TEST_CASE("shared launcher: a pack becomes the profile") {
  mode_profile profile;
  profile.mods = {"old.mod"};
  profile.pins = {{"old.mod", "2.0.0"}};
  profile.settings = {{"old.mod", {{"x", "1"}}}};
  const pack p = sample_pack();
  apply_pack_to_profile(profile, p);
  CHECK(profile.mods == p.order);
  CHECK_EQ(profile.pins.size(), std::size_t(2));
  CHECK_EQ(profile.pins.at("fixture.alpha"), std::string("1.0.0"));
  CHECK(!profile.pins.contains("old.mod"));
  CHECK(profile.map == std::optional<std::string>("ws.3735179543"));
  CHECK_EQ(profile.pack_name, p.name);
  CHECK_EQ(profile.pack_id, p.id);
  CHECK(profile.allowed_conflicts == p.decisions.allow_conflicts);
  REQUIRE_EQ(profile.settings.size(), std::size_t(1)); // old ones go
  CHECK_EQ(profile.settings.at("fixture.alpha").at("lives"), std::string("3"));

  // A pack without a map takes the map away.
  pack no_map = p;
  no_map.map.reset();
  apply_pack_to_profile(profile, no_map);
  CHECK(!profile.map.has_value());
}

TEST_CASE("shared launcher: applying a pack backs the config up first") {
  const fs::path root = fresh("pack_apply");
  launcher_paths paths;
  paths.home = root;
  paths.config = root / "config";
  write(paths.config / "launcher.json", "{\"schema\":\"xerces.config/1\"}");
  launcher_config config;
  recorded log;
  const auto backup = apply_pack(paths, config, sample_pack(), log.sink());
  REQUIRE(backup.has_value());
  CHECK(fs::exists(*backup / "config" / "launcher.json"));
  CHECK(log.has("packs.backup"));
  CHECK_EQ(config.profiles.at("zm").pack_name, std::string("Weekend Zombies"));
  // Another mode's profile when asked.
  apply_pack(paths, config, sample_pack(), log.sink(), "cp");
  CHECK_EQ(config.profiles.at("cp").mods.size(), std::size_t(2));
  std::error_code ec;
  fs::remove_all(root, ec);
}

TEST_CASE("shared launcher: the child's environment") {
  const wchar_t block[] = L"=C:=C:\\x\0Path=C:\\bin\0xerces_home=old\0"
                          L"B=1\0DROP=me\0";
  const std::wstring out =
      child_environment(block, {{L"XERCES_HOME", L"C:\\home"},
                                {L"XERCES_LAUNCH_ID", L"abc"},
                                {L"DROP", std::nullopt}});
  std::vector<std::wstring> entries;
  for (const wchar_t *at = out.c_str(); *at; at += wcslen(at) + 1) {
    entries.emplace_back(at);
  }
  const std::vector<std::wstring> expected = {
      L"=C:=C:\\x", L"B=1", L"Path=C:\\bin", L"XERCES_HOME=C:\\home",
      L"XERCES_LAUNCH_ID=abc"};
  CHECK(entries == expected);
  // Double-NUL terminated.
  REQUIRE(out.size() >= 2);
  CHECK(out[out.size() - 1] == L'\0');
  CHECK(out[out.size() - 2] == L'\0');
  // An empty block still makes a valid one.
  const std::wstring empty = child_environment(nullptr, {});
  CHECK_EQ(empty.size(), std::size_t(2));
}

TEST_CASE("shared launcher: one launcher per home") {
  const fs::path home = fresh("instance");
  CHECK_EQ(launcher_home_hash(home), launcher_home_hash(home / ""));
  std::wstring upper = home.wstring();
  for (wchar_t &c : upper) {
    c = static_cast<wchar_t>(towupper(c));
  }
  CHECK_EQ(launcher_home_hash(home), launcher_home_hash(fs::path(upper)));
  CHECK_EQ(launcher_home_hash(home),
           launcher_home_hash(fs::path(home.generic_wstring())));
  CHECK(launcher_home_hash(home) != launcher_home_hash(home / "other"));

  CHECK(!launcher_running(home));
  HANDLE mutex =
      CreateMutexW(nullptr, TRUE, launcher_instance_name(home).c_str());
  REQUIRE(mutex != nullptr);
  CHECK(launcher_running(home));
  CHECK(launcher_running(home / ""));
  ReleaseMutex(mutex);
  CloseHandle(mutex);
  CHECK(!launcher_running(home));
  std::error_code ec;
  fs::remove_all(home, ec);
}

TEST_CASE("shared launcher: xerces_cli holds the home while it edits") {
  const fs::path home = fresh("lock");
  {
    const launcher_home_lock lock(home);
    REQUIRE(lock.held());
    // A launcher opening now sees its mutex taken (and waits for it); so does
    // a second edit.
    CHECK(launcher_running(home));
    const launcher_home_lock second(home / "");
    CHECK(!second.held());
    CHECK(launcher_running(home)); // the refused lock let go of nothing
  }
  CHECK(!launcher_running(home));
  {
    // A running launcher: no lock.
    HANDLE mutex =
        CreateMutexW(nullptr, TRUE, launcher_instance_name(home).c_str());
    REQUIRE(mutex != nullptr);
    const launcher_home_lock lock(home);
    CHECK(!lock.held());
    ReleaseMutex(mutex);
    CloseHandle(mutex);
  }
  CHECK(!launcher_running(home));
  std::error_code ec;
  fs::remove_all(home, ec);
}

TEST_CASE("shared launcher: XERCES_DRY_LAUNCH") {
  wchar_t saved[64] = {};
  const DWORD had = GetEnvironmentVariableW(L"XERCES_DRY_LAUNCH", saved, 64);
  SetEnvironmentVariableW(L"XERCES_DRY_LAUNCH", L"1");
  CHECK(dry_launch_from_environment());
  SetEnvironmentVariableW(L"XERCES_DRY_LAUNCH", L"0");
  CHECK(!dry_launch_from_environment());
  SetEnvironmentVariableW(L"XERCES_DRY_LAUNCH", nullptr);
  CHECK(!dry_launch_from_environment());
  if (had > 0 && had < 64) {
    SetEnvironmentVariableW(L"XERCES_DRY_LAUNCH", saved);
  }
}

TEST_CASE("shared launcher: a game process is seen from its start") {
  // This test's own process stands in for a client of that name: what
  // matters is the name, wherever the file is.
  wchar_t self[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  const fs::path own(self);
  CHECK(game_process_running(fs::path("C:/elsewhere") / own.filename()));
  std::wstring name = own.filename().wstring();
  for (wchar_t &c : name) {
    c = static_cast<wchar_t>(towupper(c));
  }
  CHECK(game_process_running(fs::path(name))); // names ignore case
}

TEST_CASE("shared launcher: text files are written whole") {
  const fs::path root = fresh("write");
  const fs::path file = root / "sub" / "launcher.json";
  CHECK(write_text_file(file, "one"));
  CHECK(write_text_file(file, "two"));
  CHECK(read_text_file(file) == std::optional<std::string>("two"));
  CHECK(!fs::exists(root / "sub" / "launcher.json.tmp"));
  CHECK(!read_text_file(root / "missing").has_value());

  // A failed write says why and leaves no .tmp behind (services::save_config
  // shows it as config_error()). The reason is a fixed sentence a screen can
  // translate; the detail names the file for the log. A write that works
  // clears both.
  std::string why = "an older problem";
  std::string detail = "an older file";
  CHECK(write_text_file(file, "three", &why, &detail));
  CHECK(why.empty());
  CHECK(detail.empty());
  // launcher.json cannot be replaced: a folder of that name holds a file.
  const fs::path blocked = root / "blocked" / "launcher.json";
  write(blocked / "keep.txt", "x");
  CHECK(!write_text_file(blocked, "four", &why, &detail));
  CHECK_EQ(why, std::string("The old file could not be replaced."));
  CHECK(detail.starts_with("launcher.json: "));
  CHECK(detail.size() > std::string("launcher.json: ").size());
  CHECK(fs::exists(blocked / "keep.txt"));
  CHECK(!fs::exists(root / "blocked" / "launcher.json.tmp"));
  // The temporary file cannot be made: a folder holds its name.
  fs::create_directories(root / "tmp" / "launcher.json.tmp");
  CHECK(
      !write_text_file(root / "tmp" / "launcher.json", "five", &why, &detail));
  CHECK_EQ(why,
           std::string("A temporary file could not be created in its folder."));
  CHECK_EQ(detail, std::string("launcher.json.tmp"));
  CHECK(!fs::exists(root / "tmp" / "launcher.json"));
  // Without a detail, the reason alone.
  CHECK(!write_text_file(blocked, "six", &why));
  CHECK_EQ(why, std::string("The old file could not be replaced."));
  std::error_code ec;
  fs::remove_all(root, ec);
}

TEST_CASE("log tail: one line per event") {
  const std::string line =
      "{\"component\":\"launcher\",\"data\":{\"plan_id\":\"sha256:ab\"},"
      "\"event\":\"launcher.launch.dry\",\"level\":\"info\",\"msg\":\"-launch "
      "-nointro\",\"role\":\"launcher\",\"session\":\"1\",\"ts\":\"2026-11-"
      "05T09:48:22.040Z\"}";
  CHECK_EQ(format_log_line(line),
           std::string("09:48:22.040 info  launcher.launch.dry | -launch "
                       "-nointro  {\"plan_id\":\"sha256:ab\"}"));
  CHECK_EQ(format_log_line(
               "{\"event\":\"launcher.exit\",\"level\":\"error\",\"msg\":"
               "\"closed\",\"ts\":\"2026-11-05T09:48:24.039Z\"}\r"),
           std::string("09:48:24.039 error launcher.exit | closed"));
  // Not an event: as it is.
  CHECK_EQ(format_log_line("not json"), std::string("not json"));
  CHECK_EQ(format_log_line("{\"a\":1}"), std::string("{\"a\":1}"));
  // A Workshop title with escape sequences in the message (JSON decodes the
  // escaped ESC, BEL and CSI to the real characters): they print as ?.
  CHECK_EQ(format_log_line(
               "{\"event\":\"cli.mods.edit\",\"level\":\"info\",\"msg\":\""
               "\\u001b]52;c;aGk=\\u0007x is on\\u009b2J\",\"ts\":\"2026-11-"
               "05T09:48:24.039Z\"}"),
           std::string("09:48:24.039 info  cli.mods.edit | ?]52;c;aGk=?x is "
                       "on?2J"));
  CHECK_EQ(format_log_line("raw \x1b[2J line"), std::string("raw ?[2J line"));
}

TEST_CASE("terminal_safe: control characters go, text stays") {
  CHECK_EQ(terminal_safe("Pegasus \x1b[31mred\x07"),
           std::string("Pegasus ?[31mred?"));
  CHECK_EQ(terminal_safe("tab\tand\nnewline stay; cr\r del\x7f nul"),
           std::string("tab\tand\nnewline stay; cr? del? nul"));
  CHECK_EQ(terminal_safe(std::string_view("a\0b", 3)), std::string("a?b"));
  // C1 controls as UTF-8 (U+009B is CSI); other non-ASCII text is kept.
  CHECK_EQ(terminal_safe("\xc2\x9b"
                         "2J \xc2\x85"),
           std::string("?2J ?"));
  CHECK_EQ(terminal_safe("caf\xc3\xa9 \xc2\xa0 \xe6\x97\xa5\xe6\x9c\xac"),
           std::string("caf\xc3\xa9 \xc2\xa0 \xe6\x97\xa5\xe6\x9c\xac"));
  // A lone 0xC2 at the end is kept as it is (it is no control).
  CHECK_EQ(terminal_safe("x\xc2"), std::string("x\xc2"));
}

TEST_CASE("log tail: the last lines of a file, read from its end") {
  const fs::path dir = fresh("tail");
  const fs::path file = dir / "20261105-094818-launcher-1.jsonl";
  // About 2 MB of lines, so the end is many blocks away from the start.
  std::string text;
  for (int i = 0; i < 40000; ++i) {
    text += "line " + std::to_string(i) +
            " ........................................\n";
  }
  text += "half a line";
  write(file, text);
  const auto tail = tail_file(file, 3);
  REQUIRE(tail.has_value());
  const std::vector<std::string> expected = {
      "line 39997 ........................................",
      "line 39998 ........................................",
      "line 39999 ........................................"};
  CHECK(tail->lines == expected);
  CHECK_EQ(tail->end, std::uint64_t(text.size() - std::strlen("half a line")));
  // All of them, and more than there are.
  CHECK_EQ(tail_file(file, 0)->lines.size(), std::size_t(40000));
  CHECK_EQ(tail_file(file, 50000)->lines.size(), std::size_t(40000));
  // A line longer than a block, and empty lines that do not count.
  const fs::path wide = dir / "20261105-094819-launcher-2.jsonl";
  const std::string long_line(200000, 'x');
  write(wide, "first\n" + long_line + "\n\n\nlast\n");
  const auto two = tail_file(wide, 2);
  REQUIRE(two.has_value());
  REQUIRE_EQ(two->lines.size(), std::size_t(2));
  CHECK_EQ(two->lines[0], long_line);
  CHECK_EQ(two->lines[1], std::string("last"));
  CHECK_EQ(two->end, std::uint64_t(fs::file_size(wide)));
  // Nothing complete yet, an empty file, a file that is not there.
  write(wide, "no newline");
  CHECK(tail_file(wide, 5)->lines.empty());
  CHECK_EQ(tail_file(wide, 5)->end, std::uint64_t(0));
  write(wide, "");
  CHECK(tail_file(wide, 5)->lines.empty());
  CHECK(!tail_file(dir / "missing.jsonl", 5).has_value());

  CHECK_EQ(log_start(file), std::string("20261105-094818"));
  CHECK(log_start(dir / "journal.json").empty());
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST_CASE("log tail: the newest log of a role, and its last lines") {
  const fs::path logs = fresh("logs");
  write(logs / "20261105-094818-launcher-19164.jsonl", "a\n");
  write(logs / "20261105-095505-cli-45920.jsonl", "b\n");
  write(logs / "20261104-235959-launcher-1.jsonl", "c\n");
  write(logs / "20261105-100000-game-77.jsonl", "d\n");
  write(logs / "journal.json", "{}");
  write(logs / "notes.jsonl", "x\n");
  CHECK_EQ(log_role(logs / "20261105-094818-launcher-19164.jsonl"),
           std::string("launcher"));
  CHECK(log_role(logs / "journal.json").empty());
  CHECK(log_role(logs / "notes.jsonl").empty());
  CHECK(newest_log(logs, "launcher") ==
        std::optional<fs::path>(logs / "20261105-094818-launcher-19164.jsonl"));
  CHECK(newest_log(logs, "") ==
        std::optional<fs::path>(logs / "20261105-100000-game-77.jsonl"));
  CHECK(!newest_log(logs, "server").has_value());
  CHECK(todays_cli_log(logs, "20261105") ==
        std::optional<fs::path>(logs / "20261105-095505-cli-45920.jsonl"));
  CHECK(!todays_cli_log(logs, "20261106").has_value());

  const auto lines = last_lines("one\r\ntwo\n\nthree\nfour (being written)", 2);
  const std::vector<std::string> expected = {"two", "three"};
  CHECK(lines == expected);
  CHECK_EQ(last_lines("a\nb\nc\n", 0).size(), std::size_t(3));
  std::error_code ec;
  fs::remove_all(logs, ec);
}

TEST_CASE("signature report: what the game wrote at its last start") {
  const std::string report = R"({
    "schema": "xerces.sigreport/1",
    "game_build": "0x6531394",
    "checked": "2026-11-05T03:00:00Z",
    "registry": "C:/x/compat/signatures.json",
    "from_cache": true,
    "entries": [
      {"id": "db.override_asset", "name": "DB_OverrideAsset", "resolved": true, "rva": "0x1040"},
      {"id": "ui.label", "name": "ui.label", "resolved": false, "error": "not in compat/signatures.json"}
    ],
    "disabled": { "multi_mod": ["ui.label"] }
  })";
  const signature_state state = parse_signature_report(report);
  REQUIRE(state.known);
  CHECK_EQ(state.game_build, std::string("0x6531394"));
  CHECK_EQ(state.registry, std::string("C:/x/compat/signatures.json"));
  CHECK(state.from_cache);
  CHECK_EQ(state.matched, std::size_t(1));
  CHECK_EQ(state.total, std::size_t(2));
  REQUIRE_EQ(state.entries.size(), std::size_t(2));
  CHECK_EQ(state.entries[0].rva, std::string("0x1040"));
  CHECK_EQ(state.entries[1].error,
           std::string("not in compat/signatures.json"));
  CHECK(state.missing == std::vector<std::string>{"ui.label"});
  CHECK(state.multi_mod_off());
  REQUIRE_EQ(state.disabled_ids.size(), std::size_t(1));
  CHECK(state.disabled_ids[0] == std::vector<std::string>{"ui.label"});
  CHECK(state.has_miss());

  const signature_state clean = parse_signature_report(
      R"({"game_build":"0x6531394","entries":[{"id":"a","name":"A","resolved":true,"rva":"0x1"}],"disabled":{}})");
  CHECK(clean.known);
  CHECK(!clean.has_miss());

  // Nothing resolved at all (the registry was missing): a miss too.
  const signature_state broken_registry = parse_signature_report(
      R"({"problem":"compat/signatures.json is missing","entries":[]})");
  CHECK(broken_registry.has_miss());

  CHECK(!parse_signature_report("").known);
  CHECK(!parse_signature_report("[1,2]").known);
}

TEST_CASE("mods info: the details pane's facts") {
  fake_setup setup("info");
  mode_profile &zm = profile_for(setup.config, "zm");
  zm.mods = {"fixture.alpha", "ws.42"};
  zm.pins = {{"fixture.alpha", "1.0.0"}};
  const plan p = resolve_profile(setup.catalog.items, setup.config, "zm",
                                 setup.game.steam_build);
  const auto alpha = describe_mod(setup.catalog, &zm, p, "zm", "fixture.alpha");
  REQUIRE(alpha.has_value());
  CHECK(alpha->installed);
  CHECK_EQ(alpha->name, std::string("Fixture Alpha"));
  CHECK_EQ(alpha->version, std::string("1.0.0"));
  CHECK_EQ(alpha->source, std::string("mods"));
  CHECK_EQ(alpha->folder_name, std::string("fixture.alpha"));
  CHECK_EQ(alpha->position, std::size_t(1));
  CHECK_EQ(alpha->enabled, std::size_t(2));
  CHECK_EQ(alpha->pinned, std::string("1.0.0"));
  CHECK(alpha->modes == std::vector<std::string>{"zm"});
  CHECK(alpha->needs == std::vector<std::string>{"fixture.probe ^1.0.0"});
  CHECK_EQ(alpha->files, std::size_t(3));
  CHECK(alpha->content_hash.starts_with("sha256:"));
  CHECK_EQ(alpha->provides, std::size_t(1));
  CHECK_EQ(alpha->hooks, std::size_t(1));

  const auto probe = describe_mod(setup.catalog, &zm, p, "zm", "fixture.probe");
  REQUIRE(probe.has_value());
  CHECK_EQ(probe->position, std::size_t(0));
  CHECK(probe->loads_at != 0);
  CHECK(probe->dependency);

  // Enabled but not installed: the plan's problem is pinned to it.
  const auto missing = describe_mod(setup.catalog, &zm, p, "zm", "ws.42");
  REQUIRE(missing.has_value());
  CHECK(!missing->installed);
  CHECK_EQ(missing->workshop_id, std::string("42"));
  CHECK(!missing->problems.empty());

  CHECK(!describe_mod(setup.catalog, &zm, p, "zm", "nothing.here").has_value());
  // Beta is installed and off; the mp mode is not one it lists.
  const auto beta = describe_mod(setup.catalog, &zm, p, "mp", "fixture.beta");
  REQUIRE(beta.has_value());
  CHECK_EQ(beta->position, std::size_t(0));
  CHECK(!beta->supports_mode);
}

TEST_CASE("mods info: a folder name outside the code page") {
  // An accented and a Japanese name: the ANSI code page mangles the first and
  // cannot hold the second. The folder comes back as UTF-8.
  fake_setup setup("info_unicode");
  const fs::path folder = fs::path(*setup.game.dir) / "mods" /
                          fs::path(u8"zm_caf\u00e9_\u65e5\u672c");
  write(folder / "xerces.mod.json",
        R"({"schema":1,"id":"fixture.unicode","name":"Unicode Folder",)"
        R"("version":"1.0.0","license":"MIT"})");
  write(folder / "zone" / "core_mod.ff", "x");
  setup.catalog = scan_catalog(setup.game, setup.paths.cache / "hashes.txt");
  const plan p = resolve_profile(setup.catalog.items, setup.config, "zm",
                                 setup.game.steam_build);
  const auto info =
      describe_mod(setup.catalog, nullptr, p, "zm", "fixture.unicode");
  REQUIRE(info.has_value());
  CHECK(
      info->folder.ends_with("/mods/zm_caf\xc3\xa9_\xe6\x97\xa5\xe6\x9c\xac"));
  CHECK(info->folder.find('\\') == std::string::npos);
  CHECK_EQ(info->folder_name,
           std::string("zm_caf\xc3\xa9_\xe6\x97\xa5\xe6\x9c\xac"));
}
