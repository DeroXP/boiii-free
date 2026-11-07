#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "asset_limits.hpp"
#include "diagnostics.hpp"
#include "manifest.hpp"
#include "semver.hpp"
#include "zone_memory.hpp"

namespace xerces_core {
// One installed mod or map version, as the catalog sees it.
struct installed_content {
  manifest info;      // parsed, or synthesized for legacy content
  std::string folder; // what mod-stack loads: Workshop id or folder name
  std::filesystem::path path;     // absolute folder, set by the catalog
  std::vector<std::string> files; // relative paths, '/' separated
  std::string content_hash;       // "sha256:..." over the files
  bool valid_manifest = true;     // false: its xerces.mod.json has errors
  // The pool file the client reads when the mod or map loads
  // (find_pool_file), relative to path with '/' separators, empty when there
  // is none; and what it asks for (parse_pool_limits). pool_file_unread: the
  // catalog found the file but did not read it (larger than
  // max_pool_file_bytes, or unreadable), so what it asks for is unknown and
  // pool_limits is empty. In no hash.
  std::string pool_file;
  std::vector<pool_limit> pool_limits;
  bool pool_file_unread = false;
  // Every zone file (is_zone_file) and what its header asks of the DB
  // stack's main pool, read by the catalog (0x100 bytes a file, never
  // hashed), for the zone memory check (5.3 step 5). In no hash.
  zone_header_map zone_headers;
};

struct plan_request {
  std::string mode = "zm";
  std::vector<std::string> mods; // the user's order, highest priority first
  std::optional<std::string> map;
  std::map<std::string, std::string> pins; // id -> exact version (packs)
  // Declared conflicts the user allowed for this pack, as sorted id pairs.
  std::set<std::pair<std::string, std::string>> allowed_conflicts;
  std::map<std::string, std::int64_t> saved_positions; // tie-break
  // id -> setting key -> canonical JSON value
  std::map<std::string, std::map<std::string, std::string>> settings;
  std::string game_build;
  version xerces_version;
  std::uint32_t protocol_version = 1;
  std::optional<std::size_t> stock_zones; // zones the mode and map use
  // <game>\boiii_players\user\launcher_settings.json, for the pool report's
  // defaults; empty when there is none (the client's defaults). In no hash.
  std::string launcher_settings;
  // The zone memory figures of the game build and mode
  // (compat/zone_memory.json, find_zone_budget); nullopt when there are
  // none, and then whether maps fit is not checked. In no hash.
  std::optional<zone_budget> zone_budget;
};

struct plan_entry {
  std::string id;
  std::string name;
  version ver;
  std::string folder;
  std::filesystem::path path; // the chosen version's files; in no hash
  std::string content_hash;
  bool requested = false; // false: pulled in as a dependency
  bool legacy = false;
  std::size_t zones = 0;
  // What its zones take from the DB stack's main pool in the plan's mode
  // (zone_memory_of); in no hash.
  std::uint64_t zone_memory = 0;
  std::map<std::string, std::string> settings; // key -> canonical JSON
};

// Why the plan looks the way it does, one decision at a time.
struct explanation {
  std::string code; // order.dependency, order.user, order.hint, order.tie, ...
  std::string subject;
  std::string object;
  std::string message;
};

// The asset pools a plan needs (5.4 step 4), from the pool files of its mods
// and its map (the client reads a map's file when the map loads). Not part of
// plan_id or content_id.
struct pool_report {
  // launcher_settings.json lets the client size its pools (is_enabled).
  bool enabled = true;
  // Per type, the largest size a mod in the stack or the plan's map asks
  // for; on a tie, the mod higher in the stack, and a mod before the map,
  // which loads last.
  std::vector<pool_need> stack;
  // The same over every mod and map of the catalog resolve_plan was given,
  // whatever its mode. Given every installed item (the launcher's catalog of
  // Workshop, mods\ and usermaps\), it is what the game's pools are to be
  // sized with at DB_AssetPoolInit; the CLI gives it one --mods-dir. On a
  // tie, the lowest id.
  std::vector<pool_need> installed;
  // The ids whose pool file the catalog found but did not read
  // (installed_content::pool_file_unread): in the stack, in its order, then
  // the map; and in the whole catalog, sorted. What they ask for is unknown,
  // so it is missing from the lists above.
  std::vector<std::string> stack_unread;
  std::vector<std::string> installed_unread;
};

// A custom map that does not fit in the zone memory a plan leaves.
struct zone_memory_map {
  std::string id;
  std::string name;
  double need_mib = 0;    // baseline + the plan's mods + the map
  double over_by_mib = 0; // need_mib - the pool
  // It does not fit even with no mods, so removing mods does not help.
  bool over_without_mods = false;
  // The launch does not have the larger zone memory, and with it this map
  // would fit with these mods (zone_budget::larger_pool_mib).
  bool fits_larger_pool = false;
};

// An installed custom map whose fit is unknown: a zone's header was not read.
struct zone_memory_unread_map {
  std::string id;
  std::string name;
};

// Zone memory (5.3 step 5): when a custom map loads, blocks 5 and 6 of every
// loaded zone come from one fixed pool, the DB stack's main pool, and a map
// that does not fit makes the engine exit the game while it loads. What the
// plan's mods and map take, and which installed maps of the mode would not
// fit with these mods or could not be checked. Not part of plan_id or
// content_id: it depends on every installed map and on compat/zone_memory.json,
// not only on the plan.
struct zone_memory_report {
  // From the request; nullopt: no figures for the build and mode, so maps
  // are not checked and maps_over is empty.
  std::optional<zone_budget> budget;
  std::uint64_t stack_bytes = 0;          // every mod in the stack
  std::optional<std::uint64_t> map_bytes; // the plan's map, when it has one
  // Every installed custom map of the mode (the plan's own included) whose
  // zones do not fit on top of the baseline and the stack: the most over
  // first, then by id. Each map's highest installed version, or the one the
  // plan picked for its map.
  std::vector<zone_memory_map> maps_over;
  // Every installed custom map of the mode (the plan's own included, same
  // versions as above) with a zone whose header was not read and that fits
  // without that zone, so whether it fits is unknown: by id. A map over the
  // pool without the zone is in maps_over instead. Empty when not checked.
  std::vector<zone_memory_unread_map> maps_unread;
  // Ids in the stack, then the plan's map, with a zone whose header was not
  // read (zone_memory_use::unread): what those zones take is missing from
  // stack_bytes and map_bytes. Other installed maps: maps_unread.
  std::vector<std::string> unread;
};

// Something the player can do to fix a problem (9.4).
struct fix_action {
  // install, update, unpin, remove, reorder (id below detail), move_to_top,
  // allow_conflict (detail: the other mod), reset_setting (detail: the
  // setting's key), update_xerces
  std::string kind;
  std::string id;
  std::string detail;
};

struct plan {
  std::string mode;
  std::vector<plan_entry> stack; // [0] is the main mod (fs_game)
  std::optional<plan_entry> map;
  std::vector<explanation> explanations;
  std::vector<fix_action> fixes;
  // The declared conflicts this plan loads because the request allowed them
  // (5.3 step 2), as sorted id pairs; a pack made from the plan keeps them.
  // Allowed pairs that are not both in the plan, or not declared, are left
  // out.
  std::set<std::pair<std::string, std::string>> allowed_conflicts;
  diagnostics diags;
  std::size_t zones_added = 0;
  std::optional<std::size_t> zones_total;
  std::string plan_id;
  std::string content_id;
  std::string canonical; // the JSON plan_id is taken over
  pool_report pools;
  zone_memory_report zone_memory;

  bool ok() const { return !diags.has_errors(); }
  std::string fs_game() const;
  std::string fs_extramods() const;
};

inline constexpr std::size_t zone_slot_limit =
    63; // 64-slot table, one reserved

plan resolve_plan(const std::vector<installed_content> &catalog,
                  const plan_request &request);

// The plan with everything a report or the CLI shows, as JSON.
std::string plan_to_json(const plan &value, bool pretty = false);

// The zones a mod adds to the engine's table in a mode: its fastfiles
// (zone/*.ff, or *.ff at the root of a Workshop folder), one language copy
// per zone however many languages ship, and no other mode's tier (mp_mod in
// Zombies) when mode is given.
std::size_t count_zone_files(const std::vector<std::string> &files,
                             std::string_view mode = {});
} // namespace xerces_core
