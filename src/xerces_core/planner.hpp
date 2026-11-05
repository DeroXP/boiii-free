#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

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
