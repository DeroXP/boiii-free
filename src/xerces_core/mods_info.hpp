#pragma once

// What the Mods screen's details pane says about one mod or map (DESIGN.md
// 7.3), for `xerces_cli mods info`: built from the same catalog, profile and
// plan, with the same helpers (mods_attribute, mods_folder_notes,
// mods_capabilities). Headless: no UI, English labels left to the caller.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "launcher_config.hpp"
#include "launcher_state.hpp"
#include "mods_edit.hpp"
#include "planner.hpp"

namespace xerces_core {
struct mod_details {
  std::string id;
  bool installed = false; // false: an enabled id with no folder on disk
  bool is_map = false;
  std::string name;    // without colour codes; the id when it has none
  std::string version; // "1.2.0", or "legacy" for a mod without a manifest
  bool legacy = false;
  std::string source;      // "workshop", "mods" or "usermaps"
  std::string folder;      // the folder on disk, '/' separated
  std::string folder_name; // what mod-stack loads (Workshop id or folder)
  std::string workshop_id;
  std::size_t files = 0, zones = 0;
  std::string content_hash;
  std::vector<std::string> modes; // empty: every mode
  bool supports_mode = true;      // the mode the details are for
  // The mode's load order: the 1-based place (0 when off) and its length.
  std::size_t position = 0, enabled = 0;
  // The 1-based place in the resolved plan's stack, also for a mod the plan
  // pulled in as a dependency (0 when not in the plan).
  std::size_t loads_at = 0;
  bool dependency = false; // in the plan only because another mod needs it
  bool map_picked = false; // a map: the profile uses it
  std::string pinned;      // the version an applied pack pinned
  std::vector<std::string> authors;
  std::string license;
  std::size_t provides = 0, overrides = 0, extends = 0, hooks = 0;
  std::vector<std::string> needs;     // "<id> <range>", "(optional)" added
  std::vector<std::string> conflicts; // the ids it declares conflicts with
  std::vector<mods_capability> capabilities;
  std::vector<setting_decl> settings;
  // Errors and warnings, as Mods lists them: the plan's notes about it, its
  // folder's scan notes and a broken manifest.
  std::vector<mods_note> problems;
  std::vector<fix_action> fixes; // the planner's fixes for it
  std::vector<std::string> why;  // ordering decisions that name it
};

// The installed version the details show for an id: the one the plan picked,
// else the highest installed.
const installed_content *chosen_content(const catalog_state &catalog,
                                        const plan &resolved,
                                        std::string_view id);

// Nothing when id is neither installed nor in the mode's load order or map.
std::optional<mod_details> describe_mod(const catalog_state &catalog,
                                        const mode_profile *profile,
                                        const plan &resolved,
                                        const std::string &mode,
                                        const std::string &id);
} // namespace xerces_core
