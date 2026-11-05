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

namespace xerces_core {
// One installed mod or map version, as the catalog sees it.
struct installed_content {
  manifest info;      // parsed, or synthesized for legacy content
  std::string folder; // what mod-stack loads: Workshop id or folder name
  std::filesystem::path path;     // absolute folder, set by the catalog
  std::vector<std::string> files; // relative paths, '/' separated
  std::string content_hash;       // "sha256:..." over the files
  bool valid_manifest = true;     // false: its xerces.mod.json has errors
  // Mods only: the pool file the client reads when the mod loads
  // (find_pool_file), relative to path with '/' separators, empty when there
  // is none; and what it asks for (parse_pool_limits). In no hash.
  std::string pool_file;
  std::vector<pool_limit> pool_limits;
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
  std::map<std::string, std::string> settings; // key -> canonical JSON
};

// Why the plan looks the way it does, one decision at a time.
struct explanation {
  std::string code; // order.dependency, order.user, order.hint, order.tie, ...
  std::string subject;
  std::string object;
  std::string message;
};

// The asset pools a plan needs (5.4 step 4), from the mods' pool files. Not
// part of plan_id or content_id.
struct pool_report {
  // launcher_settings.json lets the client size its pools (is_enabled).
  bool enabled = true;
  // Per type, the largest size a mod in the stack asks for; on a tie, the
  // mod higher in the stack.
  std::vector<pool_need> stack;
  // The same over every installed mod, whatever its mode: what the game's
  // pools are to be sized with at DB_AssetPoolInit. On a tie, the lowest id.
  std::vector<pool_need> installed;
};

// Something the player can do to fix a problem (9.4).
struct fix_action {
  // install, update, unpin, remove, reorder, allow_conflict, update_xerces
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
  diagnostics diags;
  std::size_t zones_added = 0;
  std::optional<std::size_t> zones_total;
  std::string plan_id;
  std::string content_id;
  std::string canonical; // the JSON plan_id is taken over
  pool_report pools;

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
