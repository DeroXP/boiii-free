// Voice chat starts as push-to-talk on a new game profile (docs/DESIGN.md
// 10.5): Xerces sets it only when the game has no profile yet, keeps any
// profile that is there (the player's own choice lives in it), and tries at
// most once a session.

#include "harness.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <xerces_core/voice_default.hpp>

#define NOMINMAX
#include <Windows.h>

using namespace xerces_core;
namespace fs = std::filesystem;

namespace {
bool contains(const std::string &text, std::string_view part) {
  return text.find(part) != std::string::npos;
}

// A folder of this process's own under %TEMP%, so test runs side by side do
// not share it.
struct temp_dir {
  fs::path path;
  explicit temp_dir(const char *name)
      : path(fs::temp_directory_path() /
             (std::string(name) + "-" +
              std::to_string(::GetCurrentProcessId()))) {
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path);
  }
  ~temp_dir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

void touch(const fs::path &file) {
  fs::create_directories(file.parent_path());
  std::ofstream(file, std::ios::binary) << "x";
}

std::string lua_set(voice_state state) {
  return "Engine.SetDvar(\"" + std::string(voice_state_dvar) + "\", " +
         std::to_string(static_cast<int>(state)) + ")";
}
} // namespace

TEST_CASE("voice default: the game's profile file names") {
  CHECK(is_game_profile_file("user_settings_0.cgp"));
  CHECK(is_game_profile_file("user_settings_1.cgp"));
  CHECK(is_game_profile_file("USER_SETTINGS_0.CGP"));
  // Other files the game and BOIII keep in boiii_players are not the profile.
  CHECK(!is_game_profile_file("shoutcaster_settings_0.cgp"));
  CHECK(!is_game_profile_file("stats_mp_offline_0.cgp"));
  CHECK(!is_game_profile_file("bindings_0.cfg"));
  CHECK(!is_game_profile_file("config.ini"));
  CHECK(!is_game_profile_file("user_settings_.cgp"));
  CHECK(!is_game_profile_file("user_settings_0.cgp.bak"));
  CHECK(!is_game_profile_file(""));
}

TEST_CASE("voice default: no profile yet -> push-to-talk is due") {
  const temp_dir root("xerces-voice-none");
  // A new install: no boiii_players at all.
  CHECK(find_game_profile(root.path / "boiii_players") == game_profile::none);
  CHECK(voice_start_state(game_profile::none) == voice_state::due);
  // The folder is there, with files that are not the profile (BOIII's own,
  // or the game's that it wrote before any profile).
  const fs::path players = root.path / "boiii_players";
  touch(players / "properties.json");
  touch(players / "bindings_0.cfg");
  touch(players / "stats_mp_offline_0.cgp");
  touch(players / "user" / "config.cfg");
  CHECK(find_game_profile(players) == game_profile::none);
}

TEST_CASE("voice default: a profile is there -> the player's setting is kept") {
  const temp_dir root("xerces-voice-found");
  const fs::path players = root.path / "boiii_players";
  touch(players / "bindings_0.cfg");
  touch(players / "user_settings_0.cgp");
  CHECK(find_game_profile(players) == game_profile::found);
  CHECK(voice_start_state(game_profile::found) == voice_state::keep);
  // Whatever config.cfg says: it holds no voice choice.
  touch(players / "user" / "config.cfg");
  CHECK(find_game_profile(players) == game_profile::found);
}

TEST_CASE("voice default: a folder that cannot be read keeps the setting") {
  const temp_dir root("xerces-voice-unknown");
  // boiii_players is a file, not a folder.
  touch(root.path / "boiii_players");
  CHECK(find_game_profile(root.path / "boiii_players") ==
        game_profile::unknown);
  CHECK(voice_start_state(game_profile::unknown) == voice_state::keep);
}

TEST_CASE("voice default: the game's Lua tries once, with the client's names") {
  CHECK_EQ(voice_state_dvar, std::string_view("xerces_voice_ptt"));
  CHECK_EQ(static_cast<int>(voice_state::due), 1);
  const auto file = xerces_tests::fixtures_dir().parent_path().parent_path() /
                    "data" / "ui_scripts" / "xerces_voice" / "__init__.lua";
  REQUIRE(fs::exists(file));
  const std::string lua = xerces_tests::read_file(file);
  CHECK(contains(lua, "Dvar." + std::string(voice_state_dvar) + ":get() ~= " +
                          std::to_string(static_cast<int>(voice_state::due))));
  CHECK(contains(lua, "\"" + std::string(voice_open_mic_setting) + "\""));
  // Marked as tried before the profile is touched, so a failed attempt is
  // never repeated at a later front end; set only after the read-back.
  const std::size_t tried = lua.find(lua_set(voice_state::not_set));
  const std::size_t touched = lua.find("Engine.SetProfileVar(");
  const std::size_t attempt = lua.find("pcall(set_push_to_talk)");
  const std::size_t set = lua.find(lua_set(voice_state::set));
  REQUIRE(tried != std::string::npos);
  REQUIRE(touched != std::string::npos);
  REQUIRE(attempt != std::string::npos);
  REQUIRE(set != std::string::npos);
  CHECK(tried < touched);
  CHECK(tried < attempt);
  CHECK(attempt < set);
  // Nothing else in it moves the state.
  CHECK(lua.find("Engine.SetDvar(") == tried);
  CHECK(lua.find("Engine.SetDvar(", tried + 1) == set);
  CHECK(lua.find("Engine.SetDvar(", set + 1) == std::string::npos);
}
