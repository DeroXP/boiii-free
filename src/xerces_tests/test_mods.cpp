// The Mods screen's headless rules (src/xerces_core/mods_edit.hpp): hand
// edits of the load order, the undo history, search and which mod a plan's
// diagnostics are about.

#include "harness.hpp"

#include <string>
#include <vector>

#include <xerces_core/mods_edit.hpp>

using namespace xerces_core;

namespace {
installed_content content(const std::string &id, const std::string &name,
                          const std::string &folder) {
  installed_content out;
  out.info.id = id;
  out.info.name = name;
  out.folder = folder;
  return out;
}

launcher_config packed_config() {
  launcher_config config;
  mode_profile &zm = profile_for(config, "zm");
  zm.mods = {"ws.1", "ws.2", "ws.3"};
  zm.pins = {{"ws.1", "1.0.0"}};
  zm.pack_name = "Friday night";
  zm.pack_id = "pk_123";
  config.saved_positions = {{"ws.9", 4}};
  return config;
}
} // namespace

TEST_CASE("mods: switching a mod on puts it at the bottom") {
  mode_profile profile;
  CHECK(mods_set_enabled(profile, "ws.1", true));
  CHECK(mods_set_enabled(profile, "ws.2", true));
  CHECK((profile.mods == std::vector<std::string>{"ws.1", "ws.2"}));
  CHECK(!mods_set_enabled(profile, "ws.1", true)); // already on
  CHECK(!mods_set_enabled(profile, "", true));
  CHECK(mods_set_enabled(profile, "ws.1", false));
  CHECK((profile.mods == std::vector<std::string>{"ws.2"}));
  CHECK(!mods_set_enabled(profile, "ws.1", false)); // already off
}

TEST_CASE("mods: moving mods") {
  mode_profile profile;
  profile.mods = {"a", "b", "c", "d"};
  CHECK(mods_move(profile, 3, 0));
  CHECK((profile.mods == std::vector<std::string>{"d", "a", "b", "c"}));
  CHECK(mods_move(profile, 0, 2)); // takes the target's place
  CHECK((profile.mods == std::vector<std::string>{"a", "b", "d", "c"}));
  CHECK(!mods_move(profile, 1, 1));
  CHECK(!mods_move(profile, 4, 0));
  CHECK(!mods_move(profile, 0, 4));

  CHECK(mods_move_by(profile, "c", -1));
  CHECK((profile.mods == std::vector<std::string>{"a", "b", "c", "d"}));
  CHECK(!mods_move_by(profile, "a", -1)); // already the top
  CHECK(!mods_move_by(profile, "d", +1)); // already the bottom
  CHECK(mods_move_by(profile, "b", +10)); // stops at the bottom
  CHECK((profile.mods == std::vector<std::string>{"a", "c", "d", "b"}));
  CHECK(!mods_move_by(profile, "zz", -1));
}

TEST_CASE("mods: the map and pins") {
  mode_profile profile;
  CHECK(mods_set_map(profile, std::string("ws.map")));
  CHECK(profile.map == std::optional<std::string>("ws.map"));
  CHECK(!mods_set_map(profile, std::string("ws.map")));
  CHECK(mods_set_map(profile, std::string(""))); // empty means none
  CHECK(!profile.map);
  CHECK(!mods_set_map(profile, std::nullopt));

  profile.pins = {{"ws.1", "1.0.0"}};
  CHECK(mods_unpin(profile, "ws.1"));
  CHECK(!mods_unpin(profile, "ws.1"));
}

TEST_CASE("mods: a hand edit detaches the pack and saves positions") {
  launcher_config config = packed_config();
  mode_profile &zm = profile_for(config, "zm");
  mods_move(zm, 2, 0);
  CHECK_EQ(mods_hand_edit(config, "zm"), std::string("Friday night"));
  CHECK(zm.pins.empty());
  CHECK(zm.pack_name.empty());
  CHECK(zm.pack_id.empty());
  CHECK_EQ(config.saved_positions.at("ws.3"), 0);
  CHECK_EQ(config.saved_positions.at("ws.1"), 1);
  CHECK_EQ(config.saved_positions.at("ws.2"), 2);
  CHECK_EQ(config.saved_positions.at("ws.9"), 4); // another mode's mod stays
  CHECK_EQ(mods_hand_edit(config, "zm"), std::string(""));
}

TEST_CASE("mods: undo restores the pack and the order") {
  launcher_config config = packed_config();
  mods_history history;
  CHECK(history.undo(config, "zm") == mods_history::undo_result::nothing);

  const mods_snapshot before = mods_take(config, "zm");
  mods_move_by(profile_for(config, "zm"), "ws.3", -1);
  mods_hand_edit(config, "zm");
  history.record("zm", before, mods_take(config, "zm"));
  CHECK_EQ(history.size("zm"), 1u);
  CHECK_EQ(history.size("mp"), 0u);

  const mods_snapshot second = mods_take(config, "zm");
  mods_set_enabled(profile_for(config, "zm"), "ws.4", true);
  mods_hand_edit(config, "zm");
  history.record("zm", second, mods_take(config, "zm"));

  CHECK(history.undo(config, "mp") == mods_history::undo_result::nothing);
  CHECK(history.undo(config, "zm") == mods_history::undo_result::done);
  CHECK((config.profiles.at("zm").mods ==
         std::vector<std::string>{"ws.1", "ws.3", "ws.2"}));
  CHECK(history.undo(config, "zm") == mods_history::undo_result::done);
  const mode_profile &zm = config.profiles.at("zm");
  CHECK((zm.mods == std::vector<std::string>{"ws.1", "ws.2", "ws.3"}));
  CHECK_EQ(zm.pack_name, std::string("Friday night"));
  CHECK_EQ(zm.pins.size(), 1u);
  CHECK(!config.saved_positions.contains("ws.1"));
  CHECK(history.undo(config, "zm") == mods_history::undo_result::nothing);
}

TEST_CASE("mods: undo never overwrites a change made elsewhere") {
  launcher_config config = packed_config();
  mods_history history;
  const mods_snapshot before = mods_take(config, "zm");
  mods_set_enabled(profile_for(config, "zm"), "ws.2", false);
  mods_hand_edit(config, "zm");
  history.record("zm", before, mods_take(config, "zm"));

  // Packs applies another pack to the same mode.
  profile_for(config, "zm").mods = {"ws.7"};
  profile_for(config, "zm").pack_name = "Other";
  CHECK(history.undo(config, "zm") == mods_history::undo_result::stale);
  CHECK((config.profiles.at("zm").mods == std::vector<std::string>{"ws.7"}));
  CHECK_EQ(history.size("zm"), 0u);
}

TEST_CASE("mods: the history keeps the newest edits") {
  launcher_config config;
  mods_history history(2);
  for (int i = 0; i < 4; ++i) {
    const mods_snapshot before = mods_take(config, "zm");
    mods_set_enabled(profile_for(config, "zm"), "m" + std::to_string(i), true);
    history.record("zm", before, mods_take(config, "zm"));
  }
  history.record("zm", mods_take(config, "zm"), mods_take(config, "zm"));
  CHECK_EQ(history.size("zm"), 2u); // a no-op edit is not recorded
  CHECK(history.undo(config, "zm") == mods_history::undo_result::done);
  CHECK(history.undo(config, "zm") == mods_history::undo_result::done);
  CHECK(history.undo(config, "zm") == mods_history::undo_result::nothing);
  CHECK(
      (config.profiles.at("zm").mods == std::vector<std::string>{"m0", "m1"}));
}

TEST_CASE("mods: search matches name, id, folder and Workshop id") {
  installed_content item =
      content("ws.2891234567", "Better HUD Plus", "2891234567");
  item.info.workshop_id = 2891234567u;
  CHECK(mods_matches(item, ""));
  CHECK(mods_matches(item, "   "));
  CHECK(mods_matches(item, "hud"));
  CHECK(mods_matches(item, "BETTER plus"));
  CHECK(mods_matches(item, "2891"));
  CHECK(mods_matches(item, "ws.289"));
  CHECK(!mods_matches(item, "better zombies"));

  installed_content local = content("local.my_mod", "my_mod", "My Mod");
  CHECK(mods_matches(local, "my mod"));
  CHECK(!mods_matches(local, "ws."));
}

TEST_CASE("mods: colour codes leave names") {
  CHECK_EQ(mods_plain_name("^3[^5PZM^3] ^9PEGASUS^7 v0.2.3.9"),
           std::string("[PZM] PEGASUS v0.2.3.9"));
  CHECK_EQ(mods_plain_name("BODY^1CAM^7 - lite"),
           std::string("BODYCAM - lite"));
  CHECK_EQ(mods_plain_name("^1 ^2"), std::string("^1 ^2")); // nothing left
  CHECK_EQ(mods_plain_name("a^b^"), std::string("a^b^"));
  installed_content item = content("ws.1", "^3[^5PZM^3] ^9PEGASUS", "1");
  CHECK(mods_matches(item, "pzm pegasus"));
  CHECK(mods_matches(item, "^9peg"));
}

TEST_CASE("mods: diagnostics go to the mods they name") {
  plan p;
  p.diags.error("plan.mod.missing", "/mods", "\"ws.404\" is not installed.");
  p.diags.error("plan.conflict.declared", "",
                "Zombies+ and Perk Pack are declared incompatible.");
  p.diags.warning("plan.order.hint_ignored", "",
                  "Zombies asks for Perk Pack above Zombies+, which other "
                  "rules overrule.");
  p.diags.error("plan.setting.invalid", "/settings/ws.5/rounds",
                "\"x\" is not a valid value for Rounds's \"Rounds\".");
  p.diags.error("plan.budget.zones", "",
                "This plan needs 70 zone slots; the engine has 63.");
  p.diags.error("plan.dependency.missing", "",
                "\"lib.core\" is needed but not installed (Perk Pack needs "
                "^1.0).");

  const std::vector<std::pair<std::string, std::string>> names = {
      {"ws.1", "Zombies"},  {"ws.2", "Zombies+"}, {"ws.3", "Perk Pack"},
      {"ws.404", "ws.404"}, {"ws.5", "Rounds"},   {"ws.6", "Pack"}};
  const mods_notes notes = mods_attribute(p, names);

  REQUIRE(notes.by_id.contains("ws.404"));
  CHECK_EQ(notes.by_id.at("ws.404").size(), 1u);
  CHECK(notes.has_error("ws.404"));

  // "Zombies+" is not also "Zombies"; "Perk Pack" is not also "Pack".
  CHECK(notes.has_error("ws.2"));
  CHECK(notes.has_error("ws.3"));
  CHECK(!notes.by_id.contains("ws.6"));
  REQUIRE(notes.by_id.contains("ws.1"));
  CHECK_EQ(notes.by_id.at("ws.1").size(), 1u); // the hint warning only
  CHECK(notes.has_warning("ws.1"));
  CHECK(!notes.has_error("ws.1"));

  CHECK(notes.has_error("ws.5"));                   // from the settings path
  CHECK(notes.by_id.contains("lib.core") == false); // not a shown mod
  CHECK_EQ(notes.by_id.at("ws.3").size(), 3u); // conflict, hint, dependency

  REQUIRE(notes.general.size() == 1u);
  CHECK_EQ(notes.general[0].code, std::string("plan.budget.zones"));
}

TEST_CASE("mods: names match whole words only") {
  plan p;
  p.diags.error("plan.mode.unsupported", "", "Bo does not support mp.");
  p.diags.error("plan.mode.unsupported", "", "Bonus does not support mp.");
  const mods_notes notes =
      mods_attribute(p, {{"ws.1", "Bo"}, {"ws.2", "Bonus"}});
  CHECK_EQ(notes.by_id.at("ws.1").size(), 1u);
  CHECK_EQ(notes.by_id.at("ws.2").size(), 1u);
}

TEST_CASE("mods: scan diagnostics by folder") {
  diagnostics diags;
  diags.error("manifest.field", "123/name", "123: name is missing.");
  diags.warning("legacy.workshop_json", "123",
                "workshop.json in 123 is not valid JSON.");
  diags.warning("legacy.workshop_json", "1234",
                "workshop.json in 1234 is not valid JSON.");
  CHECK_EQ(mods_folder_notes(diags, "123").size(), 2u);
  CHECK_EQ(mods_folder_notes(diags, "1234").size(), 1u);
  CHECK(mods_folder_notes(diags, "").empty());
}

TEST_CASE("mods: short hashes and capabilities") {
  CHECK_EQ(mods_short_hash("sha256:0123456789abcdef"),
           std::string("0123456789ab"));
  CHECK_EQ(mods_short_hash("abc"), std::string("abc"));
  capabilities caps;
  CHECK(mods_capabilities(caps).empty());
  caps.native_code = true;
  caps.lua_net = {"example.com", "example.org"};
  const auto lines = mods_capabilities(caps);
  REQUIRE(lines.size() == 2u);
  CHECK_EQ(lines[1].detail, std::string("example.com, example.org"));
}
