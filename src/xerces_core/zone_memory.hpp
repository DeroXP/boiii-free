#pragma once

// Zone memory, headless (docs/DESIGN.md 5.3 step 5). When a custom map loads,
// the engine takes blocks 5 and 6 of every loaded zone (fastfile_header.hpp)
// from one fixed pool, the DB stack's main pool (2 GiB on the September 2026
// Steam build). A map whose zones do not fit on top of the game's own and the
// mods' makes the engine's out-of-memory handler exit the game while the map
// loads: players see the game vanish. compat/zone_memory.json holds the
// figures per game build; this reads it and adds up what a mod's or a map's
// zones take, so the planner can warn before a launch.

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "diagnostics.hpp"

namespace xerces_core {
// compat/zone_memory.json:
//   {"schema": "xerces.zone_memory/1", "_comment": "...",
//    "builds": [{"name": "September 2026 Steam client",
//                "steam_build": 24784313, "checksum": "0x6531394",
//                "db_main_pool_mib": 2048,
//                "baseline_mib": {"zm": 849, "mp": null, "cp": null}}]}
// A build is named by the keys compat/builds.json uses (steam_build,
// checksum; at least one). baseline_mib: per mode, what the pool holds when a
// custom map's own zones start loading with no mods; null, or left out, when
// nobody has measured it. Keys starting with "_" are comments.
inline constexpr std::string_view zone_memory_schema = "xerces.zone_memory/1";

struct zone_memory_build {
  std::string name;
  std::optional<std::uint64_t> steam_build;
  std::optional<std::uint32_t> checksum;
  double db_main_pool_mib = 0;
  std::map<std::string, double> baseline_mib; // measured modes only
};

struct zone_memory_table {
  std::vector<zone_memory_build> builds;
  diagnostics diags;
  bool ok() const { return !diags.has_errors(); }
};

zone_memory_table parse_zone_memory(std::string_view text);

// What the planner checks a plan against: one build, one mode.
struct zone_budget {
  double pool_mib = 0;
  double baseline_mib = 0;
};

// The entry for a game, by Steam's build number or else the exe's checksum;
// nullptr when none matches.
const zone_memory_build *
find_zone_memory_build(const zone_memory_table &table,
                       std::optional<std::uint64_t> steam_build,
                       std::optional<std::uint32_t> checksum = std::nullopt);

// The budget of a mode on a build; nullopt when the mode is not measured.
std::optional<zone_budget> zone_budget_for(const zone_memory_build &build,
                                           std::string_view mode);

// The same from a plan request's game_build: Steam's build number as the
// launcher writes it ("24784313"), or an exe checksum as `xerces_cli plan
// resolve --game-build` takes it ("0x6531394"). nullopt for anything else,
// an unknown build or an unmeasured mode.
std::optional<zone_budget> find_zone_budget(const zone_memory_table &table,
                                            std::string_view game_build,
                                            std::string_view mode);

// --------------------------------------------------- a mod's or map's zones
// The zone files of a folder: *.ff at its root (Steam's Workshop layout) or
// in zone/ (Mod Tools builds), as the catalog lists them (relative, '/'
// separated). Each maps to what its header asks of the main pool
// (fastfile_header::db_main_pool_bytes), or nullopt when the header could not
// be read (another format, too short, or held open by another program).
using zone_header_map = std::map<std::string, std::optional<std::uint64_t>>;

bool is_zone_file(std::string_view relative);

struct zone_memory_use {
  std::uint64_t bytes = 0;
  // Files that load but whose header was not read, so bytes leaves them
  // out.
  std::vector<std::string> unread;
};

// What a mod's (mod = true) or a map's zones take from the main pool when
// they load in a mode. A mod loads its tiers: core_mod, the mode's tier
// (zm_mod in Zombies, never another mode's) and their _patch; a map loads all
// its zones. One language copy per zone ("en_core_mod" next to "core_mod"):
// the game loads its own language's, and the largest is counted, so the sum
// never depends on the player's language and errs high by a little.
zone_memory_use zone_memory_of(const zone_header_map &zones,
                               std::string_view mode, bool mod);

// The modes a custom map is for, from its zones' names: "zm_backrooms" is a
// Zombies map, "mp_x" Multiplayer, "cp_x" Campaign. Empty when no zone says.
std::set<std::string> map_zone_modes(const zone_header_map &zones);

inline constexpr double bytes_per_mib = 1024.0 * 1024.0;
inline double to_mib(std::uint64_t bytes) {
  return static_cast<double>(bytes) / bytes_per_mib;
}
// MiB to one decimal, as reports show them.
double round_mib(double mib);
} // namespace xerces_core
