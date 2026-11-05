#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "diagnostics.hpp"

namespace xerces_core {
// The launcher's one versioned config file, <home>\config\launcher.json
// (DESIGN.md 3.5). Unknown keys are kept on a round trip, so a newer launcher's
// settings survive an older one.
inline constexpr std::string_view launcher_config_schema = "xerces.config/1";

// What the player picked for one game mode (8.4: profiles per game mode).
struct mode_profile {
  std::vector<std::string> mods; // highest priority first
  std::optional<std::string> map;
  bool modded = true; // Launch modded vs vanilla was last used
  // From an applied pack (8.3): id -> exact version, and the pack's name
  // and id; cleared when the player edits the load order by hand.
  std::map<std::string, std::string> pins;
  std::string pack_name;
  std::string pack_id;
  // The applied pack's decisions (8.1, 8.3): declared conflicts it allows, as
  // sorted id pairs, and per-mod settings (id -> key -> canonical JSON).
  // Kept on a hand edit (Mods can allow a conflict too); only the pins and
  // the pack's name and id detach.
  std::set<std::pair<std::string, std::string>> allowed_conflicts;
  std::map<std::string, std::map<std::string, std::string>> settings;
};

struct favorite_server {
  std::string name;
  std::string address; // host:port
};

struct launcher_config {
  std::optional<std::string> game_dir; // the saved path (4.7, step 1)
  bool first_run_done = false;
  std::string mode = "zm"; // zm, mp, cp
  std::map<std::string, mode_profile> profiles;
  std::map<std::string, std::int64_t> saved_positions; // planner tie-break

  // Launch (Home and Settings).
  std::string player_name;
  bool nointro = true;
  bool close_on_launch = false;
  std::vector<std::string> extra_flags;
  // Settings that become "+set <dvar> <value>": the Settings screen's game
  // and display options, by dvar name.
  std::map<std::string, std::string> dvars;

  // Look and feel (7.5).
  std::string theme = "dark"; // dark, light, contrast
  int ui_scale_percent = 0;   // 0: follow Windows; else 75 to 200
  bool reduced_motion = false;
  std::string language = "en";

  // Privacy and updates (16.6).
  bool check_updates = true;

  std::vector<favorite_server> favorites;
  std::vector<std::string> recent_servers; // host:port, newest first

  // Keys this version does not know, as canonical JSON, kept as they were.
  std::map<std::string, std::string> unknown;
};

struct launcher_config_result {
  launcher_config value;
  diagnostics diags; // a broken file gives defaults and an error here
  bool ok() const { return !diags.has_errors(); }
};

launcher_config_result parse_launcher_config(std::string_view text);
std::string launcher_config_to_json(const launcher_config &value); // pretty

// The profile for a mode, created empty on first use.
mode_profile &profile_for(launcher_config &config, const std::string &mode);
} // namespace xerces_core
