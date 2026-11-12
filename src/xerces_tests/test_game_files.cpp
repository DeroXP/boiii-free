// binds.cfg and launcher_settings.json as the client and upstream's launcher
// read them (xerces_core/game_files.hpp).

#include "harness.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// wingdi.h's GetObject macro would rename rapidjson's GetObject().
#undef GetObject

#include <filesystem>
#include <fstream>
#include <string>

#include <xerces_core/game_files.hpp>
#include <xerces_core/json.hpp>

using namespace xerces_core;

TEST_CASE("game files: binds read and written like upstream") {
  const auto binds = parse_binds("bind F \"say hi\"\n"
                                 "  BIND g give all  \r\n"
                                 "// comment\n"
                                 "bind nocommand\n"
                                 "bind f \"say again\"\n");
  REQUIRE_EQ(binds.size(), std::size_t{2});
  CHECK_EQ(binds[0].key, std::string("f"));
  CHECK_EQ(binds[0].command, std::string("say again"));
  CHECK_EQ(binds[1].key, std::string("g"));
  CHECK_EQ(binds[1].command, std::string("give all"));
  CHECK_EQ(binds_to_text(binds),
           std::string("bind f \"say again\"\nbind g \"give all\"\n"));
  CHECK(parse_binds(binds_to_text(binds)).size() == binds.size());
}

TEST_CASE("game files: a bind cannot break out of its line") {
  CHECK(valid_bind_key("mouse4"));
  CHECK(!valid_bind_key("a b"));
  CHECK(!valid_bind_key("a;b"));
  CHECK(!valid_bind_key(std::string(33, 'k')));
  CHECK(valid_bind_command("toggle cg_drawFPS; say on"));
  CHECK(!valid_bind_command("say \"x\""));
  CHECK(!valid_bind_command("a\nbind f quit"));
  CHECK(!valid_bind_command(""));
  // Writing leaves out only what would not read back as one bind.
  const std::string text = binds_to_text({{"f", "ok"},
                                          {"g", "a\nbind f quit"},
                                          {"h h", "x"},
                                          {"", "x"},
                                          {" k", "x"},
                                          {"j", ""},
                                          {"l\r", "x"}});
  CHECK_EQ(text, std::string("bind f \"ok\"\n"));
}

TEST_CASE("game files: binds already in the file survive another edit") {
  // The game saves any bind the engine took (no length limit), and a
  // hand-edited file can quote inside the command; upstream's launcher
  // writes such binds back, and so does Xerces when another bind changes.
  const std::string long_command(600, 'x');
  const std::string file = "bind f \"" + long_command +
                           "\"\n"
                           "bind g \"noclip\"\n"
                           "bind h \"say \"hi\"\"\n";
  auto binds = parse_binds(file);
  REQUIRE_EQ(binds.size(), std::size_t{3});
  CHECK(!valid_bind_command(binds[0].command)); // not one Xerces would add
  CHECK(!valid_bind_command(binds[2].command));
  CHECK_EQ(binds_to_text(binds), file);
  // Remove g, as the screen does: f and h are still there.
  std::erase_if(binds, [](const game_bind &b) { return b.key == "g"; });
  const auto after = parse_binds(binds_to_text(binds));
  REQUIRE_EQ(after.size(), std::size_t{2});
  CHECK_EQ(after[0].key, std::string("f"));
  CHECK_EQ(after[0].command, long_command);
  CHECK_EQ(after[1].key, std::string("h"));
  CHECK_EQ(after[1].command, std::string("say \"hi\""));
}

TEST_CASE("game files: the asset pool switch keeps the other settings") {
  CHECK(asset_pools_enabled(""));
  CHECK(asset_pools_enabled("{}"));
  CHECK(asset_pools_enabled(R"({"asset_limits_enabled":"1"})"));
  CHECK(!asset_pools_enabled(R"({"asset_limits_enabled":"0"})"));
  CHECK(!asset_pools_enabled(R"({"disable_asset_pools":"1"})"));

  const std::string off = set_asset_pools_enabled(
                              R"({"ap_image":"9000","friends_only":"1","disable_asset_pools":"1"})",
                              false)
                              .value_or("");
  CHECK(!asset_pools_enabled(off));
  const auto parsed = json::parse(off);
  REQUIRE(parsed.ok);
  CHECK_EQ(json::get_string(parsed.document, "ap_image").value_or(""),
           std::string("9000"));
  CHECK_EQ(json::get_string(parsed.document, "friends_only").value_or(""),
           std::string("1"));
  CHECK(!parsed.document.HasMember("disable_asset_pools"));
  CHECK(asset_pools_enabled(set_asset_pools_enabled(off, true).value_or("")));
  // No file yet, or a blank one: an object with the switch alone.
  CHECK(!asset_pools_enabled(set_asset_pools_enabled("", false).value_or("")));
  CHECK(set_asset_pools_enabled(" \r\n", true).has_value());
  // Text that is not a JSON object is not started again from {}: that wrote
  // a file with the switch alone and lost every other key for one typo.
  CHECK(!set_asset_pools_enabled("not json", true).has_value());
  CHECK(!set_asset_pools_enabled(R"({"ap_image":"9000",})", false).has_value());
  CHECK(!set_asset_pools_enabled("[1]", false).has_value());
}

TEST_CASE("game files: the pool switch leaves a file it cannot use alone") {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "xerces_game_files_pools";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const fs::path file = dir / "user" / "launcher_settings.json";

  // No file: it is made (and its folder).
  CHECK(switch_asset_pools(file, false) == pool_switch::written);
  CHECK(!asset_pools_enabled(xerces_tests::read_file(file)));

  // The other keys stay.
  {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << R"({"ap_image":"9000","asset_limits_enabled":"0"})";
  }
  CHECK(switch_asset_pools(file, true) == pool_switch::written);
  const auto kept = json::parse(xerces_tests::read_file(file));
  REQUIRE(kept.ok);
  CHECK_EQ(json::get_string(kept.document, "ap_image").value_or(""),
           std::string("9000"));
  CHECK_EQ(json::get_string(kept.document, "asset_limits_enabled").value_or(""),
           std::string("1"));

  // There but unreadable (here: opened by something that shares nothing, as
  // a scanner can). It used to count as "no file" and was replaced by a
  // file with the switch alone.
  const std::string before = xerces_tests::read_file(file);
  const HANDLE held = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  REQUIRE(held != INVALID_HANDLE_VALUE);
  CHECK(switch_asset_pools(file, false) == pool_switch::unreadable);
  CloseHandle(held);
  CHECK_EQ(xerces_tests::read_file(file), before);

  // Readable, but not a JSON object: left as it is, byte for byte.
  const std::string broken = R"({"ap_image":"9000", "friends_only":"1",})";
  {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << broken;
  }
  CHECK(switch_asset_pools(file, false) == pool_switch::not_an_object);
  CHECK_EQ(xerces_tests::read_file(file), broken);
  fs::remove_all(dir);
}

TEST_CASE("game files: a client Xerces did not start still counts as running") {
  CHECK(is_game_client("boiii.exe", ""));
  CHECK(is_game_client("BOIII.EXE", "boiii.exe"));
  CHECK(is_game_client("BlackOps3.exe", "boiii.exe"));
  CHECK(is_game_client("xerces_game.exe", "Xerces_Game.exe")); // after a rename
  CHECK(!is_game_client("BlackOps3_UnrankedDedicatedServer.exe", "boiii.exe"));
  CHECK(!is_game_client("Xerces.exe", "boiii.exe"));
  CHECK(!is_game_client("boiii.exe.bak", "boiii.exe"));
  CHECK(!is_game_client("", ""));

  // The snapshot sees every process: this one, given as the client's name.
  wchar_t module[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, module, MAX_PATH);
  const std::string self = std::filesystem::path(module).filename().string();
  const auto found = running_game_client(self);
  REQUIRE(found.has_value());
  CHECK(is_game_client(*found, self));
}
