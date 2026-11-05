#pragma once

// What a zone (fastfile) is to the mod stack, from its name alone (DESIGN.md
// 5.4 step 5, 6.10). Headless, so the rules are tested without the game:
//
// - the main mod's tiers: core_mod and the mode tiers zm_mod, mp_mod, cp_mod;
// - an extra mod's zones, renamed stack_<id>_<zone> (its tiers and any zone
//   its tiers depend on);
// - the custom map being loaded: zones named after the map;
// - everything else is the game's own (stock maps included).
//
// Each may have a language prefix ("en_zm_mod", "en_stack_<id>_core_mod",
// "en_zm_log_overunder") and a "_patch" suffix. A mode prefix looks like a
// language prefix ("zm_" in "zm_mod"), so the whole name is tried before a
// prefix is taken off: the mod stack used to read "zm_mod" as language "zm"
// plus "mod", which left the main mod's mode tier unranked.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace xerces_core {
enum class zone_kind {
  stock,     // the game's own, or anything not recognised
  main_tier, // the main mod (fs_game)
  extra,     // an extra mod (fs_extramods), `extra_index` says which
  usermap,   // the custom map being loaded
};

struct zone_class {
  zone_kind kind = zone_kind::stock;
  // For an extra: its place in `extra_ids` (0 = highest priority extra).
  std::size_t extra_index = 0;
};

bool is_mod_tier(std::string_view name); // core_mod, zm_mod, mp_mod, cp_mod

// `active_usermap` is the custom map's internal name ("zm_log_overunder"), or
// empty when no custom map is loading. `extra_ids` are the extra mods' ids in
// priority order; ids may themselves contain '_' (local mod folders), so an
// alias is matched against them rather than split.
zone_class classify_zone(std::string_view zone, std::string_view active_usermap,
                         std::span<const std::string> extra_ids);

// Which script copy wins when a zone being loaded ships a script another
// zone already provided (DESIGN.md 6.3 rule 6, "map scripts first"). The
// engine itself ranks every mod tier above a custom map's own zones, so a mod
// that ships its own version of a perk script replaces the map's copy, and
// the map's other scripts then fail to link (seen on 2026-11-05: a GSC link
// error on SANCTUM OBITUS with DS4C). With `map_first`, a custom map keeps
// its own scripts against any mod; everything else is left to the engine
// and the stack's own ranking.
enum class script_choice {
  engine,         // no opinion
  incoming_wins,  // the script being loaded replaces the one already there
  existing_stays, // the one already there stays
};
script_choice decide_script_override(zone_kind incoming, zone_kind existing,
                                     bool map_first);

// A script's halves: the server .gsc and the client .csc. They must come from
// the same source or their clientfield registrations disagree and the match
// disconnects, so map-first takes a half from a mod only when that cannot
// split the pair.
enum script_half : std::uint8_t { half_gsc = 1, half_csc = 2 };
// half_gsc, half_csc, or 0 for any other name; `base` gets the name without
// its extension.
std::uint8_t script_half_of(std::string_view name, std::string *base = nullptr);

enum class half_choice {
  take,           // the map's copy wins: no mod ships the other half, or the
                  // map ships it too
  take_tentative, // wins, but whether the map ships the other half is not
                  // known yet (its first load): a mixed pair is possible
  skip,           // the map ships only this half and a mod ships both: the
                  // mod keeps both, as the engine would
};
// `half` is the map's half of a script; `mod_halves` the halves mods ship of
// it; `map_seen` the halves the map's zones have linked so far this load;
// `map_known` the halves the map ships per its cache (nullopt: no cache).
half_choice decide_map_half(std::uint8_t half, std::uint8_t mod_halves,
                            std::uint8_t map_seen,
                            std::optional<std::uint8_t> map_known);

// The map-scripts cache: which halves of which scripts a custom map's zones
// ship, written after a load and trusted while `stamp` (the zone file's size
// and time) is unchanged. One "<base> <halves>" line per script after a
// "xerces.mapscripts/1 <stamp>" header.
using map_script_halves = std::map<std::string, std::uint8_t>;
std::string format_map_scripts(std::string_view stamp,
                               const map_script_halves &halves);
std::optional<map_script_halves> parse_map_scripts(std::string_view text,
                                                   std::string_view stamp);
} // namespace xerces_core
