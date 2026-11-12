#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "diagnostics.hpp"
#include "manifest.hpp"
#include "planner.hpp"

namespace xerces_core {
// How an asset type combines when several owners have it (DESIGN.md 6.4).
enum class asset_strategy {
  merge_table,      // stringtables: row and key merge
  merge_config,     // .cfg, .json and key=value rawfiles: merge by key
  merge_strings,    // localized strings: merge by key
  union_list,       // precache lists
  exclusive_script, // .gsc/.csc: one owner, hooks chain (6.6)
  exclusive,        // everything else: one owner, by order
};

const char *to_string(asset_strategy value);
asset_strategy strategy_for(std::string_view type, std::string_view name);
// Types whose content is hashed exactly, so identical copies are not conflicts.
bool has_exact_hash(std::string_view type);

// The winner when the game's own copy is used. Not a valid mod id, so no mod
// can be taken for the game.
inline constexpr std::string_view stock_owner = "@stock";

struct asset_record {
  std::string type;
  std::string name; // normalized (normalize_asset_name)
  std::string zone; // zone file, or "loose"
  std::uint64_t size = 0;
  std::string content_hash; // empty for types without an exact hash
};

// What one owner (a mod, the map or "stock") contributes, from an index run
// (6.1) or, for loose files and manifests, from the files themselves.
struct asset_index {
  std::string owner;
  std::vector<asset_record> records;
  std::vector<std::string> missing_tiers; // tiers not indexed yet
};

enum class asset_outcome {
  added,            // rule 1
  merged,           // rule 2
  override_allowed, // rule 3: declared override
  override_blocked, // rule 3: undeclared override, the next owner wins
  legacy_conflict,  // rule 4: needs a decision, saved in the pack
  identical,        // rule 5
  overlap,          // rule 5: not compared, resolved by order
  map_priority,     // rule 6
};

const char *to_string(asset_outcome value);

// One entry per asset; an asset with a blocked override can get a second
// entry for what happens among the owners from the winner down.
struct asset_ownership {
  std::string type;
  std::string name;
  asset_strategy strategy = asset_strategy::exclusive;
  std::vector<std::string> owners;  // effective order, highest first
  std::string winner;               // stock_owner: the game's copy
  std::vector<std::string> blocked; // owners whose override was blocked
  asset_outcome outcome = asset_outcome::added;
  int rule = 1;
  bool needs_decision = false;
  bool in_stock = false;
  std::string message;
};

struct conflict_inputs {
  const plan *resolved = nullptr;
  std::map<std::string, manifest> manifests; // id -> manifest
  std::vector<asset_index> indexes;          // one per owner
  std::optional<asset_index> stock;
  // Rule 3: mods the user lets override without a declaration.
  std::set<std::string> allow_undeclared;
  // Rule 4: decisions already saved in the pack, as "type:name".
  std::set<std::string> decided;
  bool map_first_for_scripts = true; // rule 6
};

struct conflict_report {
  std::vector<asset_ownership> entries; // assets with more than one owner,
                                        // or replacing stock
  std::size_t decisions = 0;
  std::size_t merges = 0;
  std::size_t blocked = 0;
  std::size_t overlaps = 0;
  std::size_t identical = 0;
  std::size_t overrides = 0;
  diagnostics diags;
  std::vector<fix_action> fixes;

  bool needs_decision() const { return decisions > 0; }
};

conflict_report check_conflicts(const conflict_inputs &inputs);
std::string conflict_report_to_json(const conflict_report &report,
                                    bool pretty = false);
// The pre-launch wording of DESIGN.md 6.7.
std::string conflict_report_to_text(const conflict_report &report);

// Builds an index from a mod folder's loose files (fixtures, the loose loader).
asset_index index_loose_files(
    const std::string &owner,
    const std::vector<std::pair<std::string, std::string>> &files);
// A file of a mod folder (relative path, either slash) that is no loose
// asset: the manifest, Workshop metadata, previews and notes, everything
// under zone/, and fastfiles and their packs (.ff, .xpak, .sabl, .sabs)
// wherever they sit.
bool loose_index_skips(std::string_view relative);
// index_loose_files for a folder on disk. Only the files the index uses are
// read: the others (loose_index_skips) can be gigabytes each.
asset_index index_loose_folder(const std::string &owner,
                               const std::filesystem::path &folder);
} // namespace xerces_core
