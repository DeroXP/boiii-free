// The engine's out-of-memory exit as the game client reports it (DESIGN.md
// 5.6, 5.7): the zone that was loading, the mods, and what the player reads.

#include "harness.hpp"

#include <string>
#include <vector>

#include <xerces_core/out_of_memory.hpp>

using namespace xerces_core;

namespace {
bool contains(const std::string &text, std::string_view part) {
  return text.find(part) != std::string::npos;
}
} // namespace

TEST_CASE("out of memory: the zone is the last 'Loading fastfile' line") {
  // The end of the console on 2026-11-05, DS4C with zm_backrooms.
  const std::vector<std::string> lines = {
      "Loading fastfile 'en_zm_backrooms'",
      "Added XPaks for en_zm_backrooms, 0ms",
      "FastFileLoad: Setting en_zm_backrooms to XZONE_LOADED in 216ms.",
      "FastFileLoad: Setting zm_backrooms to XZONE_LOADING.",
      "FastFileLoad: zm_backrooms, FastFilePriority: 1407",
      "Loading fastfile 'zm_backrooms'",
  };
  CHECK_EQ(last_loading_zone(lines), std::string("zm_backrooms"));

  // Later lines without the marker do not hide it; color codes before it are
  // fine.
  const std::vector<std::string> later = {
      "^7Loading fastfile 'stack_3171632315_core_mod'",
      "SOUND load state for core_mod.all changed from 0 to 1",
      "Redundant xmodel asset 'x' found in zone 'a' while loading zone 'b'",
  };
  CHECK_EQ(last_loading_zone(later), std::string("stack_3171632315_core_mod"));
}

TEST_CASE("out of memory: no zone line, or a cut one, gives what is known") {
  CHECK(last_loading_zone({}).empty());
  const std::vector<std::string> none = {"FastFileLoad: zm_x, priority 1",
                                         "Loading sounds for 'zm_x'"};
  CHECK(last_loading_zone(none).empty());
  // A line cut before its closing quote, or an empty name: the one before.
  const std::vector<std::string> cut = {"Loading fastfile 'zm_mod'",
                                        "Loading fastfile ''",
                                        "Loading fastfile 'zm_backro"};
  CHECK_EQ(last_loading_zone(cut), std::string("zm_mod"));
  // Game text may hold CP-1252 bytes; the name comes back as valid UTF-8.
  const std::vector<std::string> bad = {"Loading fastfile 'zm_\x92x'"};
  CHECK_EQ(last_loading_zone(bad), std::string("zm_\xEF\xBF\xBDx"));
}

TEST_CASE("out of memory: mods by their names from the plan, else folders") {
  const std::string plan = R"({
    "plan_id": "sha256:7fb8",
    "fs_game": "1944930083",
    "fs_extramods": "3171632315,3656779213",
    "stack": [
      {"folder": "1944930083", "id": "ws.1944930083", "name": "Solo Easter Eggs"},
      {"folder": "3171632315", "id": "ws.3171632315", "name": " DS4C Patched v0.85 "},
      {"folder": "MyLocalMod", "id": "local.mylocalmod", "name": ""}
    ]
  })";
  const auto names = read_plan_mod_names(plan);
  CHECK_EQ(names.size(), std::size_t(2));

  const auto mods =
      loaded_mods("1944930083", "3171632315, ,mylocalmod,1944930083,", names);
  REQUIRE(mods.size() == 3u);
  CHECK_EQ(mods[0], std::string("Solo Easter Eggs"));
  CHECK_EQ(mods[1], std::string("DS4C Patched v0.85"));
  CHECK_EQ(mods[2], std::string("mylocalmod")); // no name in the plan

  // DS4C alone, without a plan (started without the launcher).
  const auto alone = loaded_mods("3171632315", "");
  REQUIRE(alone.size() == 1u);
  CHECK_EQ(alone[0], std::string("3171632315"));
  CHECK(loaded_mods("", "").empty());
  CHECK(loaded_mods(" ", ",,").empty());

  // Names are matched without regard to letter case and cut to 100 bytes.
  const auto long_name =
      read_plan_mod_names(R"({"stack":[{"folder":"Big","name":")" +
                          std::string(150, 'n') + R"("}]})");
  const auto big = loaded_mods("BIG", "", long_name);
  REQUIRE(big.size() == 1u);
  CHECK_EQ(big[0].size(), std::size_t(100));

  CHECK(read_plan_mod_names("not json").empty());
  CHECK(read_plan_mod_names(R"({"stack":"no"})").empty());
  CHECK(read_plan_mod_names(R"([1,2])").empty());
}

TEST_CASE("out of memory: what the player reads with mods loaded") {
  const std::vector<std::string> mods = {"DS4C Patched v0.85"};
  const auto text = out_of_memory_text("zm_backrooms", mods);
  CHECK_EQ(text.title, std::string("Black Ops III ran out of memory"));
  CHECK(contains(text.body, "ran out of the memory it keeps for loaded "
                            "content while loading zm_backrooms."));
  CHECK(contains(text.body, "Mods loaded: DS4C Patched v0.85\n"));
  CHECK(contains(text.body, "Maps and mods share that memory, so remove a "
                            "mod"));
  CHECK(contains(text.body, "Xerces' Mods screen lists which custom maps fit "
                            "with your mods"));
  CHECK(contains(text.body, "or play another map."));
  CHECK(contains(text.body, "The details are in Xerces' game log."));
  CHECK_EQ(text.summary,
           std::string("Out of memory while loading zm_backrooms (mods: DS4C "
                       "Patched v0.85)"));

  const std::vector<std::string> two = {"Solo Easter Eggs", "DS4C"};
  CHECK(contains(out_of_memory_text("zm_x", two).body,
                 "Mods loaded: Solo Easter Eggs, DS4C\n"));
}

TEST_CASE("out of memory: no mods, or no zone known") {
  const auto alone = out_of_memory_text("zm_huge", {});
  CHECK(contains(alone.body, "Mods loaded: none\n"));
  CHECK(!contains(alone.body, "remove a mod"));
  CHECK(contains(alone.body, "Play another map."));
  CHECK_EQ(alone.summary,
           std::string("Out of memory while loading zm_huge (no mods)"));

  const std::vector<std::string> mods = {"DS4C"};
  const auto unknown = out_of_memory_text("", mods);
  CHECK(!contains(unknown.body, "while loading"));
  CHECK(contains(unknown.body, "for loaded content.\n"));
  CHECK_EQ(unknown.summary, std::string("Out of memory (mods: DS4C)"));
}
