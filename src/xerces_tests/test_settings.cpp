// The Settings screen's headless helpers (docs/DESIGN.md 7.3, 14): the player
// name and the client's properties file, the managed launch flags and the
// game options.

#include "harness.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <xerces_core/launch.hpp>
#include <xerces_core/settings_options.hpp>

using namespace xerces_core;
namespace fs = std::filesystem;

namespace {
fs::path fresh_game(const std::string &name) {
  const fs::path dir = fs::temp_directory_path() / ("xerces_settings_" + name);
  fs::remove_all(dir);
  fs::create_directories(dir / "boiii_players");
  return dir;
}

void write_bytes(const fs::path &path, const std::string &bytes) {
  std::ofstream(path, std::ios::binary) << bytes;
}

std::string read_bytes(const fs::path &path) {
  return xerces_tests::read_file(path);
}
} // namespace

TEST_CASE("settings: player names are cleaned the way the client reads them") {
  CHECK_EQ(clean_player_name("  Dero  ").name, std::string("Dero"));
  const player_name_result accents = clean_player_name("D\xC3\xA9ro^1X");
  CHECK_EQ(accents.name, std::string("Dro^1X"));
  CHECK(accents.dropped);
  CHECK(!accents.shortened);
  const player_name_result long_name =
      clean_player_name("abcdefghijklmno pqrstu");
  CHECK_EQ(long_name.name, std::string("abcdefghijklmno"));
  CHECK(long_name.shortened);
  CHECK_EQ(clean_player_name("\t\n").name, std::string());
  CHECK(!clean_player_name("\t a \n").dropped);
}

TEST_CASE("settings: the properties file keeps every other member") {
  const std::string text =
      "\xEF\xBB\xBF{\"playerName\":\"Old\",\"selectedVersion\":\"beta\","
      "\"launchOptions\":\"-windowed\"}";
  CHECK_EQ(read_player_name(text).value_or(""), std::string("Old"));

  const properties_edit renamed = with_player_name(text, "New \"One\"");
  REQUIRE(renamed.ok);
  CHECK_EQ(read_player_name(renamed.text).value_or(""),
           std::string("New \"One\""));
  CHECK(renamed.text.find("\"selectedVersion\":\"beta\"") != std::string::npos);
  CHECK(renamed.text.find("\"launchOptions\":\"-windowed\"") !=
        std::string::npos);
  CHECK(renamed.text.rfind("\xEF\xBB\xBF", 0) != 0); // written without a BOM

  const properties_edit cleared = with_player_name(text, "");
  REQUIRE(cleared.ok);
  CHECK(!read_player_name(cleared.text).has_value());
  CHECK(cleared.text.find("selectedVersion") != std::string::npos);

  // Duplicate keys go, as the client's own store() removes them.
  const properties_edit dup =
      with_player_name("{\"playerName\":\"a\",\"playerName\":\"b\"}", "c");
  REQUIRE(dup.ok);
  CHECK_EQ(dup.text, std::string("{\"playerName\":\"c\"}"));

  CHECK(with_player_name("", "x").ok);
  CHECK(!with_player_name("[1,2]", "x").ok);
  CHECK(!with_player_name("{broken", "x").ok);
  CHECK(!read_player_name("{\"playerName\":5}").has_value());
}

TEST_CASE("settings: the name goes into an existing properties file only") {
  const fs::path game = fresh_game("name");
  const fs::path file = player_properties_file(game);
  CHECK_EQ(apply_player_name(game, "Dero").status, name_apply::no_file);
  CHECK(!fs::exists(file));

  write_bytes(file, "{\"playerName\":\"Steam\",\"selectedVersion\":\"latest\"}");
  CHECK_EQ(apply_player_name(game, "Dero").status, name_apply::written);
  CHECK_EQ(read_player_name(read_bytes(file)).value_or(""),
           std::string("Dero"));
  CHECK(read_bytes(file).find("selectedVersion") != std::string::npos);
  CHECK(!fs::exists(fs::path(file).concat(".xerces-tmp")));
  CHECK_EQ(apply_player_name(game, "Dero").status, name_apply::unchanged);

  CHECK_EQ(apply_player_name(game, "").status, name_apply::written);
  CHECK(!read_player_name(read_bytes(file)).has_value());
  CHECK_EQ(apply_player_name(game, "").status, name_apply::unchanged);

  // A file Xerces cannot read is left alone, and the screen learns why.
  write_bytes(file, "not json");
  const name_apply_result broken = apply_player_name(game, "Dero");
  CHECK_EQ(broken.status, name_apply::failed);
  CHECK(broken.failure == name_failure::not_json);
  CHECK(!broken.message.empty());
  CHECK_EQ(read_bytes(file), std::string("not json"));

  write_bytes(file, "[\"a\"]");
  const name_apply_result list = apply_player_name(game, "Dero");
  CHECK_EQ(list.status, name_apply::failed);
  CHECK(list.failure == name_failure::not_object);
  CHECK_EQ(read_bytes(file), std::string("[\"a\"]"));

  // A file the new copy cannot be written next to (the temporary name is
  // taken by a folder) is left alone too.
  write_bytes(file, "{\"playerName\":\"Steam\",\"selectedVersion\":\"latest\"}");
  fs::create_directories(fs::path(file).concat(".xerces-tmp"));
  const name_apply_result blocked = apply_player_name(game, "Dero");
  CHECK_EQ(blocked.status, name_apply::failed);
  CHECK(blocked.failure == name_failure::write_failed);
  CHECK_EQ(read_player_name(read_bytes(file)).value_or(""), std::string("Steam"));
  CHECK(read_bytes(file).find("selectedVersion") != std::string::npos);
  CHECK(apply_player_name(game, "Steam").failure == name_failure::none);
  fs::remove_all(game);
}

TEST_CASE("settings: Enter on an empty name takes the file's name out") {
  // "Leave it empty to use your Steam name": a name the file holds (the
  // game's own, upstream's launcher's) differs from an empty saved name.
  CHECK(game_name_differs("", std::string("OldName")));
  CHECK(!game_name_differs("", std::nullopt));
  CHECK(!game_name_differs("", std::string(""))); // the client reads it as none
  CHECK(game_name_differs("Dero", std::string("Other")));
  CHECK(game_name_differs("Dero", std::nullopt));
  CHECK(!game_name_differs("Dero", std::string("Dero")));

  const fs::path game = fresh_game("empty_name");
  const fs::path file = player_properties_file(game);
  write_bytes(file, "{\"playerName\":\"OldName\"}");
  CHECK(game_name_differs("", read_player_name(read_bytes(file))));
  CHECK_EQ(apply_player_name(game, "").status, name_apply::written);
  CHECK(!read_player_name(read_bytes(file)).has_value());
  CHECK(!game_name_differs("", read_player_name(read_bytes(file))));
  fs::remove_all(game);
}

TEST_CASE("settings: the game's saved skip switches are read from config.cfg") {
  CHECK_EQ(game_config_file("C:/Game"),
           fs::path("C:/Game") / "boiii_players" / "user" / "config.cfg");
  // As the client writes it: set name "value", one per line.
  const std::string config =
      "set r_vsync \"0\"\r\n"
      "set boiii_skipIntro \"1\"\r\n"
      "set boiii_skipCinematics \"0\"\r\n"
      "set boiii_skipCinematicsX \"1\"\r\n";
  CHECK_EQ(read_saved_dvar(config, dvar_skip_intro).value_or("?"), std::string("1"));
  CHECK(saved_dvar_on(config, dvar_skip_intro).value_or(false));
  CHECK(!saved_dvar_on(config, dvar_skip_cinematics).value_or(true));
  CHECK(!saved_dvar_on(config, "boiii_skipForestCinematic").has_value());
  CHECK(!saved_dvar_on("", dvar_skip_intro).has_value());

  // The last line wins, names in any case, seta and unquoted values count,
  // other commands and lines without a value do not.
  const std::string edited =
      "seta BOIII_SKIPCINEMATICS 1\n"
      "bind F1 \"set boiii_skipCinematics 0\"\n"
      "set boiii_skipIntro \"1\"\n"
      "set boiii_skipIntro false\n"
      "set\n"
      "set boiii_skipCinematics  ";
  CHECK_EQ(read_saved_dvar(edited, dvar_skip_cinematics).value_or("?"),
           std::string("1"));
  CHECK(saved_dvar_on(edited, dvar_skip_cinematics).value_or(false));
  CHECK(!saved_dvar_on(edited, dvar_skip_intro).value_or(true));
  CHECK(!saved_dvar_on("set boiii_skipIntro \"\"", dvar_skip_intro).value_or(true));
}

TEST_CASE("settings: launch flags are checked like the command line does") {
  const flags_parse parsed =
      parse_launch_flags("  -windowed\t--console -windowed bad +set -launch "
                         "-a;b -xerces-plan -ultrawide ");
  CHECK((parsed.flags ==
         std::vector<std::string>{"-windowed", "--console", "-ultrawide"}));
  CHECK((parsed.invalid == std::vector<std::string>{"bad", "+set", "-a;b"}));
  CHECK((parsed.reserved == std::vector<std::string>{"-launch", "-xerces-plan"}));
  CHECK(!parsed.too_many);
  CHECK_EQ(join_flags(parsed.flags), std::string("-windowed --console -ultrawide"));
  CHECK(parse_launch_flags("").flags.empty());

  std::string many;
  for (int i = 0; i < 40; ++i) {
    many += " -f" + std::to_string(i);
  }
  const flags_parse capped = parse_launch_flags(many);
  CHECK_EQ(capped.flags.size(), max_launch_flags);
  CHECK(capped.too_many);

  // Every flag the parser keeps is one the command line accepts.
  launch_options options;
  options.extra_flags = parsed.flags;
  CHECK(build_launch_command(nullptr, options).ok());
  CHECK(!valid_launch_flag("-"));
  CHECK(!valid_launch_flag("-" + std::string(64, 'a')));
}

TEST_CASE("settings: display mode and cinematics are client flags") {
  std::vector<std::string> flags = {"-console"};
  CHECK_EQ(display_mode(flags), std::string("game"));
  flags = with_display_mode(flags, "windowed");
  CHECK((flags == std::vector<std::string>{"-console", "-windowed"}));
  CHECK_EQ(display_mode(flags), std::string("windowed"));
  flags = with_display_mode(flags, "borderless");
  CHECK((flags == std::vector<std::string>{"-console", "-borderless"}));
  flags = with_display_mode(flags, "game");
  CHECK((flags == std::vector<std::string>{"-console"}));
  // The client checks borderless first, and accepts "--".
  CHECK_EQ(display_mode({"-windowed", "--borderless"}), std::string("borderless"));
  CHECK_EQ(display_mode({"--windowed"}), std::string("windowed"));
  CHECK_EQ(display_mode({"-windowedx"}), std::string("game"));

  flags = with_flag(flags, "nocinematics", true);
  CHECK(has_flag(flags, "nocinematics"));
  flags.push_back("--nocinematics");
  flags = with_flag(flags, "nocinematics", false);
  CHECK(!has_flag(flags, "nocinematics"));
  CHECK((flags == std::vector<std::string>{"-console"}));
}

TEST_CASE("settings: -nc is the client's short -nocinematics") {
  // flags.cpp registers -nc, -nocinematics and --nocinematics as one flag,
  // so the switch reads On for -nc, and Off takes -nc away too.
  std::vector<std::string> flags = {"-nc", "-console"};
  CHECK(has_flag(flags, "nocinematics"));
  CHECK(!has_flag({"--nc"}, "nocinematics")); // not a spelling the client takes
  CHECK(!has_flag({"-nc"}, "nointro"));
  flags = with_flag(flags, "nocinematics", false);
  CHECK((flags == std::vector<std::string>{"-console"}));
  flags = with_flag({"-nc", "-console"}, "nocinematics", true);
  CHECK((flags == std::vector<std::string>{"-console", "-nocinematics"}));
  CHECK(has_flag(parse_launch_flags("-nc").flags, "nocinematics"));
}

TEST_CASE("settings: numbers and the FPS slider") {
  CHECK(parse_number_input("", 0, 10).state == number_input::kind::empty);
  CHECK(parse_number_input("  ", 0, 10).state == number_input::kind::empty);
  const number_input ok = parse_number_input(" 7 ", 0, 10);
  CHECK(ok.state == number_input::kind::ok);
  CHECK_EQ(ok.value, 7);
  CHECK(parse_number_input("11", 0, 10).state == number_input::kind::invalid);
  CHECK(parse_number_input("-1", 0, 10).state == number_input::kind::invalid);
  CHECK(parse_number_input("1.5", 0, 10).state == number_input::kind::invalid);
  CHECK(parse_number_input("+5", 0, 10).state == number_input::kind::invalid);
  CHECK(parse_number_input("99999999999", 0, 10).state ==
        number_input::kind::invalid);

  CHECK_EQ(fps_slider_index(std::nullopt), 0);
  CHECK(!fps_slider_value(0).has_value());
  CHECK_EQ(fps_slider_index(0), fps_slider_max());
  CHECK_EQ(fps_slider_value(fps_slider_max()).value_or(-1), 0);
  CHECK_EQ(fps_slider_value(1).value_or(-1), 30);
  CHECK_EQ(fps_slider_value(fps_slider_index(240)).value_or(-1), 240);
  CHECK_EQ(fps_slider_value(fps_slider_index(237)).value_or(-1), 240);
  CHECK_EQ(fps_slider_value(fps_slider_index(1000)).value_or(-1), 500);
  CHECK_EQ(fps_slider_value(fps_slider_index(1)).value_or(-1), 30);
  // Every stop goes round the slider unchanged.
  for (int i = 0; i <= fps_slider_max(); ++i) {
    CHECK_EQ(fps_slider_index(fps_slider_value(i)), i);
  }
}
