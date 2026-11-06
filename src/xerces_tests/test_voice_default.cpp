// Voice chat starts as push-to-talk in a Xerces install (docs/DESIGN.md 10.5):
// Xerces sets it once, while config.cfg has no sign that it did, and keeps
// the player's own choice after that.

#include "harness.hpp"

#include <filesystem>
#include <string>
#include <string_view>

#include <xerces_core/voice_default.hpp>

using namespace xerces_core;

namespace {
bool contains(const std::string &text, std::string_view part) {
  return text.find(part) != std::string::npos;
}
} // namespace

TEST_CASE("voice default: set while config.cfg lacks the dvar") {
  // A new install: no config.cfg yet, or an empty one.
  CHECK(voice_push_to_talk_due(""));
  // config.cfg as the client writes it, from a game without Xerces' dvar
  // (BOIII, or a Xerces build before this one).
  const std::string boiii = "set cg_fov_default \"80\"\n"
                            "set voice_input_device \"\"\n"
                            "set com_maxfps \"0\"\n";
  CHECK(voice_push_to_talk_due(boiii));
  // Saved off: the client wrote config.cfg before the game's Lua had set
  // push-to-talk (the game closed early, or ran with -nouiscripts).
  CHECK(voice_push_to_talk_due(boiii +
                               "set xerces_voice_default_applied \"0\"\n"));
  // A dvar whose name only starts the same does not count.
  CHECK(voice_push_to_talk_due("set xerces_voice_default_applied_x \"1\"\n"));
}

TEST_CASE("voice default: the player's choice is kept once Xerces has set it") {
  const std::string config = "set cg_fov_default \"80\"\n"
                             "set xerces_voice_default_applied \"1\"\n"
                             "set voice_input_device \"\"\n";
  CHECK(!voice_push_to_talk_due(config));
  // Read the way the client runs the file: names in any case, seta, no
  // quotes, CRLF line ends, the last line wins.
  CHECK(!voice_push_to_talk_due("seta XERCES_VOICE_DEFAULT_APPLIED 1\r\n"));
  CHECK(voice_push_to_talk_due(config +
                               "set xerces_voice_default_applied \"0\"\n"));
  CHECK(!voice_push_to_talk_due("set xerces_voice_default_applied \"0\"\n"
                                "set xerces_voice_default_applied \"1\"\n"));
}

TEST_CASE("voice default: the game's Lua uses the client's names") {
  CHECK_EQ(voice_default_dvar,
           std::string_view("xerces_voice_default_applied"));
  const auto file = xerces_tests::fixtures_dir().parent_path().parent_path() /
                    "data" / "ui_scripts" / "xerces_voice" / "__init__.lua";
  REQUIRE(std::filesystem::exists(file));
  const std::string lua = xerces_tests::read_file(file);
  CHECK(contains(lua, "\"" + std::string(voice_default_dvar) + "\""));
  CHECK(contains(lua, "Dvar." + std::string(voice_due_dvar)));
  CHECK(contains(lua, "\"" + std::string(voice_due_dvar) + "\""));
  CHECK(contains(lua, "\"" + std::string(voice_open_mic_setting) + "\""));
}
