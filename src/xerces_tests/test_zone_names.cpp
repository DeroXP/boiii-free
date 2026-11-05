// Zone classes for the mod stack and the map-first script rule (DESIGN.md 5.4
// step 5, 6.3 rule 6, 6.10), with the zone names seen in the 2026-11-05
// custom-map runs.

#include "harness.hpp"

#include <string>
#include <vector>

#include <xerces_core/zone_names.hpp>

using namespace xerces_core;

namespace {
const std::vector<std::string> extras{"3171632315", "3656779213"};
constexpr std::string_view map = "zm_log_overunder";

zone_kind kind_of(std::string_view zone, std::string_view usermap = map) {
  return classify_zone(zone, usermap, extras).kind;
}
} // namespace

TEST_CASE("zones: the main mod's tiers, with language and patch variants") {
  for (const char *zone :
       {"core_mod", "en_core_mod", "zm_mod", "zm_mod_patch", "en_zm_mod",
        "en_zm_mod_patch", "mp_mod", "cp_mod", "core_mod_patch"}) {
    CHECK(kind_of(zone) == zone_kind::main_tier);
  }
  // The old rule read "zm_mod" as language "zm" plus "mod".
  CHECK(kind_of("zm_mod") == zone_kind::main_tier);
  CHECK(!is_mod_tier("mod"));
}

TEST_CASE("zones: an extra mod's renamed zones, in stack order") {
  const zone_class first =
      classify_zone("stack_3171632315_zm_mod", map, extras);
  CHECK(first.kind == zone_kind::extra);
  CHECK(first.extra_index == 0u);
  const zone_class second =
      classify_zone("en_stack_3656779213_core_mod", map, extras);
  CHECK(second.kind == zone_kind::extra);
  CHECK(second.extra_index == 1u);
  CHECK(kind_of("stack_3171632315_core_mod_patch") == zone_kind::extra);
  // A zone the extra's tier depends on keeps its owner.
  CHECK(kind_of("stack_3171632315_ds4c_weapons") == zone_kind::extra);
  // An alias for a mod that is not in the stack is nobody's.
  CHECK(kind_of("stack_1404915347_core_mod") == zone_kind::stock);
  // One id being the start of another does not confuse them.
  const std::vector<std::string> similar{"123", "1234"};
  const zone_class longer = classify_zone("stack_1234_core_mod", "", similar);
  CHECK(longer.kind == zone_kind::extra);
  CHECK(longer.extra_index == 1u);
  // Local mod folders can have '_' in their id.
  const std::vector<std::string> local{"my_mod"};
  CHECK(classify_zone("stack_my_mod_zm_mod", "", local).kind ==
        zone_kind::extra);
  // One local id the start of another, in either stack order: the longest
  // id that matches owns the zone (as the zone's files are found).
  const std::vector<std::string> nested{"my", "other", "my_mod"};
  CHECK(classify_zone("stack_my_mod_zm_mod", "", nested).extra_index == 2u);
  CHECK(classify_zone("stack_my_zm_mod", "", nested).extra_index == 0u);
  CHECK(classify_zone("en_stack_my_mod_core_mod", "", nested).extra_index ==
        2u);
  const std::vector<std::string> reversed{"my_mod", "my"};
  CHECK(classify_zone("stack_my_mod_zm_mod", "", reversed).extra_index == 0u);
  CHECK(classify_zone("stack_my_zm_mod", "", reversed).extra_index == 1u);
}

TEST_CASE("zones: the custom map's own zones, and stock maps") {
  CHECK(kind_of("zm_log_overunder") == zone_kind::usermap);
  CHECK(kind_of("en_zm_log_overunder") == zone_kind::usermap);
  CHECK(kind_of("zm_log_overunder_patch") == zone_kind::usermap);
  CHECK(kind_of("zm_levelcommon") == zone_kind::stock);
  CHECK(kind_of("en_zm_levelcommon") == zone_kind::stock);
  CHECK(kind_of("zm_zod") == zone_kind::stock);
  // Another custom map's name is not the one loading.
  CHECK(kind_of("zm_ccube") == zone_kind::stock);
  // No custom map loading: map zones are stock.
  CHECK(kind_of("zm_log_overunder", "") == zone_kind::stock);
  CHECK(kind_of("") == zone_kind::stock);
  CHECK(kind_of("core_frontend") == zone_kind::stock);
}

TEST_CASE("scripts: a custom map keeps its own scripts against any mod") {
  using enum zone_kind;
  // The map's copy replaces a mod's (SANCTUM OBITUS with DS4C).
  CHECK(decide_script_override(usermap, extra, true) ==
        script_choice::incoming_wins);
  CHECK(decide_script_override(usermap, main_tier, true) ==
        script_choice::incoming_wins);
  // A mod loading after the map does not replace it.
  CHECK(decide_script_override(extra, usermap, true) ==
        script_choice::existing_stays);
  CHECK(decide_script_override(main_tier, usermap, true) ==
        script_choice::existing_stays);
  // Everything else is left to the engine and the stack's ranking: the map
  // against the game's own scripts, mods against each other, stock maps.
  CHECK(decide_script_override(usermap, stock, true) == script_choice::engine);
  CHECK(decide_script_override(stock, usermap, true) == script_choice::engine);
  CHECK(decide_script_override(extra, main_tier, true) ==
        script_choice::engine);
  CHECK(decide_script_override(main_tier, extra, true) ==
        script_choice::engine);
  CHECK(decide_script_override(stock, extra, true) == script_choice::engine);
  // Turned off (the pack's map_scripts_first false): the engine decides.
  CHECK(decide_script_override(usermap, extra, false) == script_choice::engine);
  CHECK(decide_script_override(extra, usermap, false) == script_choice::engine);
}

TEST_CASE("scripts: halves of a name") {
  std::string base;
  CHECK(script_half_of("scripts/zm/_zm_perk_staminup.gsc", &base) == half_gsc);
  CHECK(base == "scripts/zm/_zm_perk_staminup");
  CHECK(script_half_of("scripts/zm/_zm_perk_staminup.csc") == half_csc);
  CHECK(script_half_of("ui/uieditor/menus/hud/t7hud_zm_custom.lua") == 0);
  CHECK(script_half_of(".gsc") == 0);
}

TEST_CASE("scripts: map-first never splits a script's halves") {
  // No mod ships the other half: the map's copy wins (SANCTUM's _zm_perks.gsc
  // against DS4C, whose .csc stays the game's or the map's).
  CHECK(decide_map_half(half_gsc, half_gsc, 0, std::nullopt) ==
        half_choice::take);
  // The map ships both halves (KABOOM's perk scripts): known from this load
  // or from the cache.
  CHECK(decide_map_half(half_gsc, half_gsc | half_csc, half_csc,
                        std::nullopt) == half_choice::take);
  CHECK(decide_map_half(half_csc, half_gsc | half_csc, 0,
                        std::uint8_t(half_gsc | half_csc)) ==
        half_choice::take);
  // The map ships only the .gsc and the mod both: the mod keeps both.
  CHECK(decide_map_half(half_gsc, half_gsc | half_csc, 0,
                        std::uint8_t(half_gsc)) == half_choice::skip);
  CHECK(decide_map_half(half_csc, half_gsc | half_csc, 0, std::uint8_t(0)) ==
        half_choice::skip);
  // First load, nothing known yet: tentative.
  CHECK(decide_map_half(half_gsc, half_gsc | half_csc, 0, std::nullopt) ==
        half_choice::take_tentative);
}

TEST_CASE("scripts: the map-scripts cache round trip and its stamp") {
  map_script_halves halves;
  halves["scripts/zm/_zm_perk_staminup"] = half_gsc | half_csc;
  halves["scripts/zm/_zm_perks"] = half_gsc;
  halves["bad name"] = half_gsc; // never written
  const std::string text = format_map_scripts("123:456", halves);
  const auto back = parse_map_scripts(text, "123:456");
  REQUIRE(back.has_value());
  CHECK(back->size() == 2u);
  CHECK(back->at("scripts/zm/_zm_perk_staminup") == (half_gsc | half_csc));
  CHECK(back->at("scripts/zm/_zm_perks") == half_gsc);
  // Another version of the map (size or time changed), or not a cache.
  CHECK(!parse_map_scripts(text, "123:457").has_value());
  CHECK(!parse_map_scripts("", "123:456").has_value());
  CHECK(!parse_map_scripts(text, "").has_value());
  CHECK(!parse_map_scripts("garbage\nscripts/x 3\n", "123:456").has_value());
}
