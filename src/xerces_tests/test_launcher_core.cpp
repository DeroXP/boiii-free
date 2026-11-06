// The headless parts of the launcher (docs/DESIGN.md 5.4, 3.5, 7.3): the game
// command line, the config file and the catalog's hash cache.

#include "harness.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <xerces_core/catalog.hpp>
#include <xerces_core/journal.hpp>
#include <xerces_core/launch.hpp>
#include <xerces_core/launcher_config.hpp>
#include <xerces_core/manifest.hpp>
#include <xerces_core/sha256.hpp>

using namespace xerces_core;
namespace fs = std::filesystem;

namespace {
plan two_mod_plan() {
  plan p;
  p.mode = "zm";
  p.plan_id = "sha256:" + std::string(64, 'a');
  plan_entry top;
  top.id = "ws.111";
  top.folder = "111";
  plan_entry second;
  second.id = "local.hud";
  second.folder = "better hud";
  p.stack = {top, second};
  return p;
}

fs::path fresh_dir(const std::string &name) {
  const fs::path dir = fs::temp_directory_path() / ("xerces_launcher_" + name);
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

void write_bytes(const fs::path &path, const std::string &bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << bytes;
}
} // namespace

TEST_CASE("launch: flags come before the mod dvars") {
  const plan p = two_mod_plan();
  launch_options options;
  options.safe = true;
  options.extra_flags = {"-windowed"};
  options.dvars = {{"name", "Player One"}};
  const launch_command cmd = build_launch_command(&p, options);
  REQUIRE(cmd.ok());
  const std::vector<std::string> expected = {
      "-launch",      "-nointro",     "-safe",   "-windowed",
      "-xerces-plan", std::string(64, 'a'),       "+set",
      "fs_game",      "111",          "+set",    "fs_extramods",
      "better hud",   "+set",         "name",    "Player One"};
  CHECK(cmd.args == expected);
  CHECK_EQ(cmd.command_line,
           std::string("-launch -nointro -safe -windowed -xerces-plan ") +
               std::string(64, 'a') +
               " +set fs_game 111 +set fs_extramods \"better hud\" +set name "
               "\"Player One\"");
  CHECK_EQ(flag_value(cmd.args, "-xerces-plan").value_or(""),
           std::string(64, 'a'));
}

TEST_CASE("launch: vanilla and safe mode carry no mods") {
  const launch_command vanilla = build_launch_command(nullptr, {});
  REQUIRE(vanilla.ok());
  CHECK((vanilla.args == std::vector<std::string>{"-launch", "-nointro"}));

  const plan p = two_mod_plan();
  launch_options off;
  off.mods_off = true;
  const launch_command safe = build_launch_command(&p, off);
  REQUIRE(safe.ok());
  CHECK(safe.command_line.find("fs_game") == std::string::npos);
  CHECK(safe.command_line.find("-xerces-plan") == std::string::npos);
}

TEST_CASE("launch: values that could inject commands are refused") {
  plan p = two_mod_plan();
  p.stack[1].folder = "evil +set sv_cheats 1";
  CHECK(build_launch_command(&p, {}).diags.has("launch.folder.invalid"));
  p.stack[1].folder = "quote\"name";
  CHECK(build_launch_command(&p, {}).diags.has("launch.folder.invalid"));

  launch_options options;
  options.dvars = {{"name", "a +exec evil"}};
  CHECK(build_launch_command(nullptr, options)
            .diags.has("launch.dvar.invalid"));
  options.dvars = {{"bad name", "x"}};
  CHECK(build_launch_command(nullptr, options)
            .diags.has("launch.dvar.invalid"));
  options.dvars = {};
  options.extra_flags = {"+set"};
  CHECK(build_launch_command(nullptr, options).diags.has("launch.flag.invalid"));
  options.extra_flags = {"-a b"};
  const launch_command refused = build_launch_command(nullptr, options);
  CHECK(!refused.ok());
  CHECK(refused.args.empty());

  // Settings can never set the mod dvars behind the plan's back.
  launch_options sneaky;
  sneaky.dvars = {{"fs_game", "123"}};
  const launch_command cmd = build_launch_command(nullptr, sneaky);
  REQUIRE(cmd.ok());
  CHECK(cmd.command_line.find("fs_game") == std::string::npos);
}

TEST_CASE("launch: ';' and '//' never reach the command line") {
  // The engine's command buffer runs what follows a ';' as another command,
  // and its tokenizer drops the rest after "//". Values are refused rather
  // than quoted: how the engine reads quotes in "+set" is not verified.
  plan p = two_mod_plan();
  p.stack[1].folder = "zm_pack;unbindall";
  const launch_command folder = build_launch_command(&p, {});
  CHECK(folder.diags.has("launch.folder.invalid"));
  CHECK(folder.args.empty());

  launch_options options;
  options.dvars = {{"com_maxfps", "1;disconnect"}};
  const launch_command value = build_launch_command(nullptr, options);
  CHECK(value.diags.has("launch.dvar.invalid"));
  CHECK(value.command_line.empty());
  options.dvars = {{"name", "a//b"}};
  CHECK(
      build_launch_command(nullptr, options).diags.has("launch.dvar.invalid"));
  options.dvars = {{"name", "one/two"}}; // a single slash is fine
  CHECK(build_launch_command(nullptr, options).ok());

  launch_options join;
  join.connect = "10.0.0.1:27017;quit";
  CHECK(
      build_launch_command(nullptr, join).diags.has("launch.connect.invalid"));
}

TEST_CASE("launch: settings cannot reach the mod dvars by case or hash") {
  // The engine finds dvars by a hash of the lower-cased name, and takes
  // "0x<hash>" as that hash, so both reach fs_game and fs_extramods.
  const plan p = two_mod_plan();
  launch_options options;
  options.dvars = {{"FS_GAME", "2846283723"}, {"Fs_ExtraMods", "1,2"}};
  const launch_command modded = build_launch_command(&p, options);
  REQUIRE(modded.ok());
  CHECK(modded.command_line.find("FS_GAME") == std::string::npos);
  CHECK(modded.command_line.find("Fs_ExtraMods") == std::string::npos);
  CHECK(modded.command_line.find("2846283723") == std::string::npos);

  // Safe mode still launches without any mod.
  options.mods_off = true;
  const launch_command safe = build_launch_command(&p, options);
  REQUIRE(safe.ok());
  CHECK((safe.args == std::vector<std::string>{"-launch", "-nointro"}));

  launch_options hashed;
  hashed.dvars = {{"0x1a2b3c4d", "2846283723"}};
  CHECK(build_launch_command(nullptr, hashed).diags.has("launch.dvar.invalid"));
  hashed.dvars = {{"0X1A2B3C4D", "1"}};
  CHECK(build_launch_command(nullptr, hashed).diags.has("launch.dvar.invalid"));
  // A digit first is still a name when it is not "0x".
  hashed.dvars = {{"0r_mode", "1"}};
  CHECK(build_launch_command(nullptr, hashed).ok());
}

TEST_CASE("launch: a plan with errors or no id does not launch") {
  plan p = two_mod_plan();
  p.diags.error("plan.test", "", "broken");
  CHECK(build_launch_command(&p, {}).diags.has("launch.plan.errors"));
  plan q = two_mod_plan();
  q.plan_id = "sha256:XYZ";
  CHECK(build_launch_command(&q, {}).diags.has("launch.plan.id"));
  CHECK_EQ(plan_file_id("sha256:abc"), std::string("abc"));
}

TEST_CASE("launch: a server address goes last as +connect") {
  launch_options options;
  options.connect = "192.168.1.20:27017";
  const launch_command cmd = build_launch_command(nullptr, options);
  REQUIRE(cmd.ok());
  CHECK_EQ(cmd.args.back(), std::string("192.168.1.20:27017"));
  CHECK_EQ(cmd.args[cmd.args.size() - 2], std::string("+connect"));

  CHECK(valid_server_address("host.example:28960"));
  CHECK(valid_server_address("10.0.0.1"));
  CHECK(valid_server_address("[::1]:27017"));
  CHECK(!valid_server_address("a b:1"));
  CHECK(!valid_server_address("x:0"));
  CHECK(!valid_server_address("x:70000"));
  CHECK(!valid_server_address("x;quit"));
  CHECK(!valid_server_address("+connect"));
  CHECK(!valid_server_address("-flag"));
  options.connect = "evil +quit";
  CHECK(build_launch_command(nullptr, options)
            .diags.has("launch.connect.invalid"));
}

TEST_CASE("launch: the game reads the plan's dvars back from the plan file") {
  plan p = two_mod_plan();
  plan_entry third;
  third.id = "ws.333";
  third.folder = "333";
  p.stack.push_back(third);
  const auto back = read_plan_mod_dvars(plan_to_json(p, true));
  REQUIRE(back.has_value());
  CHECK_EQ(back->plan_id, p.plan_id);
  CHECK_EQ(back->fs_game, p.fs_game());
  CHECK_EQ(back->fs_extramods, p.fs_extramods());
  CHECK(!read_plan_mod_dvars("{}").has_value());
  CHECK(!read_plan_mod_dvars("not json").has_value());
}

TEST_CASE("launch: arguments quote the way CommandLineToArgvW reads them") {
  CHECK_EQ(quote_argument("plain"), std::string("plain"));
  CHECK_EQ(quote_argument(""), std::string("\"\""));
  CHECK_EQ(quote_argument("a b"), std::string("\"a b\""));
  CHECK_EQ(quote_argument("C:\\Game Dir\\"),
           std::string("\"C:\\Game Dir\\\\\""));
  CHECK_EQ(quote_argument("say \"hi\""), std::string("\"say \\\"hi\\\"\""));
}

TEST_CASE("manifest: colour codes leave display titles") {
  CHECK_EQ(strip_color_codes("^6DS4C ^5Patched ^7v0.85"),
           std::string("DS4C Patched v0.85"));
  CHECK_EQ(strip_color_codes("^3[^5PZM^3] ^9PEGASUS^7 v0.2"),
           std::string("[PZM] PEGASUS v0.2"));
  CHECK_EQ(strip_color_codes("Plain ^ caret"), std::string("Plain ^ caret"));
  CHECK_EQ(strip_color_codes("^1"), std::string("^1"));
}

TEST_CASE("safe mode: a culprit search survives between launches") {
  culprit_progress progress;
  progress.mode = "zm";
  progress.crashed_stack = {"a", "b", "c", "d"};
  progress.search = culprit_search(progress.crashed_stack);
  // The lower part first: it needs nothing from above it.
  progress.testing = progress.search.next_test();
  CHECK((progress.testing == std::vector<std::string>{"c", "d"}));
  progress.search.record(false); // c, d ran fine
  const auto back = parse_culprit_progress(culprit_progress_to_json(progress));
  REQUIRE(back.has_value());
  CHECK_EQ(back->mode, std::string("zm"));
  CHECK(back->crashed_stack == progress.crashed_stack);
  CHECK(back->testing == progress.testing);
  culprit_search search = back->search;
  CHECK((search.next_test() == std::vector<std::string>{"a", "b"}));
  CHECK_EQ(search.launches(), std::size_t{1});
  search.record(true); // a, b crash
  CHECK((search.next_test() == std::vector<std::string>{"b"}));
  search.record(false);
  CHECK((search.next_test() == std::vector<std::string>{"a"}));
  search.record(true);
  REQUIRE(search.culprit().has_value());
  CHECK_EQ(*search.culprit(), std::string("a"));
  CHECK(!parse_culprit_progress("{}").has_value());
}

TEST_CASE("safe mode: the larger zone memory step survives between launches") {
  culprit_progress progress;
  progress.mode = "zm";
  progress.crashed_stack = {"a", "b"};
  progress.search = culprit_search(progress.crashed_stack, true);
  progress.testing = progress.search.next_test();
  CHECK((progress.testing == std::vector<std::string>{"a", "b"}));
  auto back = parse_culprit_progress(culprit_progress_to_json(progress));
  REQUIRE(back.has_value());
  CHECK(back->search.testing_db_pool());
  CHECK(back->testing == progress.testing);

  progress.search.record(true); // the stack crashed without it too
  progress.testing.clear();
  back = parse_culprit_progress(culprit_progress_to_json(progress));
  REQUIRE(back.has_value());
  CHECK(back->search.db_pool() == db_pool_suspect::cleared);
  CHECK((back->search.next_test() == std::vector<std::string>{"b"}));
  CHECK_EQ(back->search.launches(), std::size_t{1});

  progress.search = culprit_search(progress.crashed_stack, true);
  progress.search.record(false); // it ran fine without it
  back = parse_culprit_progress(culprit_progress_to_json(progress));
  REQUIRE(back.has_value());
  CHECK(back->search.db_pool_culprit());
  CHECK(back->search.done());
  CHECK(!back->search.culprit().has_value());

  // Saved before the setting was a suspect; a state this version does not
  // know is not read.
  const std::string older =
      R"({"schema":1,"mode":"zm","crashed_stack":["a","b"],"testing":[],)"
      R"("suspects":["a","b"],"second_half":false,"combination":false,)"
      R"("launches":0})";
  const auto old = parse_culprit_progress(older);
  REQUIRE(old.has_value());
  CHECK(old->search.db_pool() == db_pool_suspect::none);
  CHECK((old->search.next_test() == std::vector<std::string>{"b"}));
  // Saved without "ran_fine": no test known to have run fine.
  CHECK(!old->search.ran_fine());
  // That search tested the upper half first; "second_half" said it had run
  // fine, so the other half is what is left to test.
  std::string half = older;
  half.replace(half.find(R"("second_half":false)"), 19,
               R"("second_half":true)");
  const auto upper_fine = parse_culprit_progress(half);
  REQUIRE(upper_fine.has_value());
  CHECK((upper_fine->search.cleared() == std::vector<std::string>{"a"}));
  CHECK((upper_fine->search.next_test() == std::vector<std::string>{"b"}));
  CHECK(upper_fine->search.ran_fine()); // the cleared half ran fine
  std::string unknown = older;
  unknown.insert(unknown.size() - 1, R"(,"db_pool":"maybe")");
  CHECK(!parse_culprit_progress(unknown).has_value());
}

TEST_CASE("config: round trip keeps known and unknown keys") {
  launcher_config c;
  c.game_dir = "D:\\SteamLibrary\\steamapps\\common\\Call of Duty Black Ops III";
  c.first_run_done = true;
  c.mode = "mp";
  profile_for(c, "zm").mods = {"ws.1", "ws.2"};
  profile_for(c, "zm").map = "ws.9";
  profile_for(c, "mp").modded = false;
  profile_for(c, "zm").pins = {{"ws.1", "1.2.0"}};
  profile_for(c, "zm").pack_name = "Weekend";
  profile_for(c, "zm").pack_id = "id-1";
  profile_for(c, "zm").allowed_conflicts = {{"a.mod", "b.mod"}};
  profile_for(c, "zm").settings["a.mod"]["mult"] = "1.5";
  c.saved_positions = {{"ws.1", 3}};
  c.player_name = "Dero";
  c.extra_flags = {"-windowed"};
  c.dvars = {{"com_maxfps", "237"}};
  c.theme = "contrast";
  c.ui_scale_percent = 150;
  c.reduced_motion = true;
  c.favorites = {{"Night Shift", "10.0.0.2:27017"}};
  c.recent_servers = {"10.0.0.2:27017"};
  c.unknown["future_key"] = R"({"a":1})";

  const std::string text = launcher_config_to_json(c);
  const launcher_config_result back = parse_launcher_config(text);
  REQUIRE(back.ok());
  const launcher_config &b = back.value;
  CHECK(b.game_dir == c.game_dir);
  CHECK(b.first_run_done);
  CHECK_EQ(b.mode, std::string("mp"));
  REQUIRE(b.profiles.contains("zm"));
  CHECK(b.profiles.at("zm").mods == c.profiles.at("zm").mods);
  CHECK(b.profiles.at("zm").map == std::optional<std::string>("ws.9"));
  CHECK(!b.profiles.at("mp").modded);
  CHECK_EQ(b.profiles.at("zm").pins.at("ws.1"), std::string("1.2.0"));
  CHECK_EQ(b.profiles.at("zm").pack_name, std::string("Weekend"));
  CHECK_EQ(b.profiles.at("zm").pack_id, std::string("id-1"));
  CHECK((b.profiles.at("zm").allowed_conflicts ==
         std::set<std::pair<std::string, std::string>>{{"a.mod", "b.mod"}}));
  CHECK_EQ(b.profiles.at("zm").settings.at("a.mod").at("mult"),
           std::string("1.5"));
  CHECK_EQ(b.saved_positions.at("ws.1"), std::int64_t{3});
  CHECK_EQ(b.player_name, std::string("Dero"));
  CHECK(b.extra_flags == c.extra_flags);
  CHECK_EQ(b.dvars.at("com_maxfps"), std::string("237"));
  CHECK_EQ(b.theme, std::string("contrast"));
  CHECK_EQ(b.ui_scale_percent, 150);
  CHECK(b.reduced_motion);
  REQUIRE_EQ(b.favorites.size(), std::size_t{1});
  CHECK_EQ(b.favorites[0].address, std::string("10.0.0.2:27017"));
  CHECK(b.recent_servers == c.recent_servers);
  CHECK_EQ(b.unknown.at("future_key"), std::string(R"({"a":1})"));
  CHECK_EQ(launcher_config_to_json(b), text);
}

TEST_CASE("config: bad values fall back to defaults") {
  const auto broken = parse_launcher_config("{ not json");
  CHECK(!broken.ok());
  CHECK_EQ(broken.value.mode, std::string("zm"));

  const auto odd = parse_launcher_config(
      R"({"mode":"xx","theme":"neon","ui_scale_percent":500,
          "profiles":{"zz":{"mods":["a"]}},"game_dir":""})");
  REQUIRE(odd.ok());
  CHECK_EQ(odd.value.mode, std::string("zm"));
  CHECK_EQ(odd.value.theme, std::string("dark"));
  CHECK_EQ(odd.value.ui_scale_percent, 0);
  CHECK(odd.value.profiles.empty());
  CHECK(!odd.value.game_dir.has_value());
}

TEST_CASE("catalog: the hash cache rehashes only changed files") {
  const fs::path dir = fresh_dir("hash_cache");
  const fs::path mods = dir / "mods";
  write_bytes(mods / "111" / "zone" / "core_mod.ff", std::string(5000, 'a'));
  write_bytes(mods / "111" / "zone" / "workshop.json",
              R"({"Title":"One","PublisherID":"111","Type":"mod"})");
  write_bytes(mods / "222" / "zone" / "core_mod.ff", "second");
  write_bytes(mods / "222" / "zone" / "workshop.json",
              R"({"Title":"Two","PublisherID":"222","Type":"mod"})");

  const catalog_scan plain = scan_mods_directory(mods);
  hash_cache cache;
  scan_options options;
  options.cache = &cache;
  std::vector<std::string> seen;
  options.progress = [&](std::size_t, std::size_t total,
                         const std::string &name) {
    CHECK_EQ(total, std::size_t{2});
    seen.push_back(name);
  };
  const catalog_scan first = scan_mods_directory(mods, options);
  // Folders are reported as their files are done, in no fixed order.
  std::ranges::sort(seen);
  CHECK((seen == std::vector<std::string>{"111", "222"}));
  CHECK_EQ(cache.misses(), std::size_t{4});
  CHECK_EQ(cache.hits(), std::size_t{0});
  REQUIRE_EQ(first.catalog.size(), plain.catalog.size());
  for (std::size_t i = 0; i < first.catalog.size(); ++i) {
    CHECK_EQ(first.catalog[i].content_hash, plain.catalog[i].content_hash);
  }

  const fs::path file = dir / "cache" / "hashes.txt";
  REQUIRE(cache.save(file));
  hash_cache reloaded;
  REQUIRE(reloaded.load(file));
  CHECK_EQ(reloaded.size(), std::size_t{4});

  // Change one file's contents and size: only it is read again.
  write_bytes(mods / "222" / "zone" / "core_mod.ff", "second, changed");
  options.cache = &reloaded;
  options.progress = nullptr;
  const catalog_scan second = scan_mods_directory(mods, options);
  CHECK_EQ(reloaded.misses(), std::size_t{1});
  CHECK_EQ(reloaded.hits(), std::size_t{3});
  const catalog_scan fresh = scan_mods_directory(mods);
  REQUIRE_EQ(second.catalog.size(), fresh.catalog.size());
  CHECK_EQ(second.catalog[1].content_hash, fresh.catalog[1].content_hash);
  CHECK(second.catalog[1].content_hash != first.catalog[1].content_hash);

  // Pruning drops entries no scan used since load, under a folder the scan
  // read in full.
  fs::remove_all(mods / "111");
  hash_cache pruned;
  REQUIRE(pruned.load(file));
  options.cache = &pruned;
  scan_mods_directory(mods, options);
  REQUIRE(pruned.save(file, {mods}));
  hash_cache after;
  REQUIRE(after.load(file));
  CHECK_EQ(after.size(), std::size_t{2});

  // A cache file from elsewhere is ignored, not trusted.
  write_bytes(file, "not a cache\n1\t2\t3\tx\n");
  hash_cache foreign;
  CHECK(!foreign.load(file));
  CHECK_EQ(foreign.size(), std::size_t{0});
  fs::remove_all(dir);
}

TEST_CASE("catalog: a cancelled scan stops between files") {
  const fs::path dir = fresh_dir("cancel");
  for (const char *id : {"1", "2", "3"}) {
    write_bytes(dir / id / "zone" / "workshop.json",
                std::string(R"({"Title":"T","PublisherID":")") + id +
                    R"(","Type":"mod"})");
  }
  std::atomic<bool> cancel = false;
  scan_options options;
  options.cancel = &cancel;
  options.hash_threads = 1; // one file at a time: the cancel lands in order
  options.progress = [&](std::size_t index, std::size_t, const std::string &) {
    if (index == 1) {
      cancel = true;
    }
  };
  const catalog_scan scan = scan_mods_directory(dir, options);
  CHECK(scan.cancelled);
  CHECK_EQ(scan.catalog.size(), std::size_t{2});
  fs::remove_all(dir);
}
