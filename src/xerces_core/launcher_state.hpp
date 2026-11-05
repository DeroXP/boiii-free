#pragma once

// What the launcher knows about the PC, shared with xerces_cli so both see
// the same game and the same catalog (DESIGN.md 4.2, 4.3, 4.7): the game
// folder and its pre-flight check, the catalog of the Workshop folder and the
// game's mods\ and usermaps\ (with the hash cache in <home>\cache), and the
// game's last signature report. Headless: no UI.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "compat.hpp"
#include "diagnostics.hpp"
#include "planner.hpp"

namespace xerces_core {
struct game_state {
  bool searched = false;
  std::optional<std::filesystem::path> dir; // the Black Ops III folder
  std::vector<std::string> tried;
  std::optional<std::uint64_t> steam_build; // appmanifest "buildid"
  std::optional<exe_facts> exe;
  std::optional<preflight_result> preflight;
  std::string error; // why the exe or the registry could not be read
  std::filesystem::path
      workshop_dir;               // <library>\steamapps\workshop\content\311210
  std::filesystem::path mods_dir; // <game>\mods
  std::filesystem::path usermaps_dir; // <game>\usermaps
  bool can_launch() const {
    return dir && preflight && preflight->can_launch();
  }
};

// Detection (4.7): the saved path, then Steam's libraries; then Steam's build
// number from the appmanifest and, when preflight is true, the pre-flight
// check (4.2) of BlackOps3.exe against <compat_dir>\builds.json. The
// pre-flight hashes BlackOps3.exe (about 100 MB); without it can_launch() is
// false.
game_state detect_game_state(const std::optional<std::string> &saved_dir,
                             const std::filesystem::path &compat_dir,
                             bool preflight = true);

struct catalog_state {
  bool scanning = false;
  bool scanned = false; // at least one full scan finished
  std::size_t done = 0, total = 0;
  std::string current; // folder being read
  // Workshop items, then the game's mods and usermaps folders.
  std::vector<installed_content> items;
  diagnostics diags;
  std::size_t cache_hits = 0, cache_misses = 0;
  std::uint64_t bytes_hashed = 0;
  double seconds = 0;
  // Where each item came from: "workshop", "mods" or "usermaps", by index.
  std::vector<std::string> sources;
  // The highest installed version of an id.
  const installed_content *find(std::string_view id) const;
};

// Called before each folder: the source ("workshop", "mods", "usermaps"),
// the folder's index and count in that source, and its name.
using catalog_progress =
    std::function<void(const std::string &source, std::size_t index,
                       std::size_t total, const std::string &name)>;

// Reads the found game's Workshop folder, mods\ and usermaps\ with the hash
// cache in cache_file, and saves the cache afterwards (pruned to what this
// scan saw, unless it was cancelled). Names lose the game's colour codes
// (ids and hashes do not, so plans do not change). scanned is false when it
// was cancelled. Nothing is read when the game was not found.
// The game folder's boiii_players\user\launcher_settings.json (the asset
// pool switch and sizes the client reads), or "" without a game or file.
std::string read_launcher_settings(const game_state &game);

catalog_state scan_catalog(const game_state &game,
                           const std::filesystem::path &cache_file,
                           const std::atomic<bool> *cancel = nullptr,
                           const catalog_progress &progress = {});

// The game's last signature report, <home>\cache\signatures-report.json
// (written by the game role each time it resolves the registry, 4.3).
struct signature_entry {
  std::string id;
  std::string name;
  bool resolved = false;
  std::string rva;   // when resolved
  std::string error; // when not
};

struct signature_state {
  bool known = false; // a report was read
  std::size_t matched = 0, total = 0;
  std::string game_build;
  std::string checked;  // UTC time
  std::string registry; // the registry file the game used
  bool from_cache = false;
  std::vector<signature_entry> entries;
  std::vector<std::string> missing;  // names that did not resolve
  std::vector<std::string> disabled; // features the game turned off
  // Ids each turned-off feature lacked, in the order of disabled.
  std::vector<std::vector<std::string>> disabled_ids;
  std::string problem; // why nothing resolved (registry missing, ...)
  bool multi_mod_off() const {
    return std::find(disabled.begin(), disabled.end(), "multi_mod") !=
           disabled.end();
  }
  // Something did not resolve, or nothing could be resolved at all.
  bool has_miss() const {
    return !missing.empty() || !problem.empty() || !disabled.empty();
  }
};

// known is false when the text is not a JSON object.
signature_state parse_signature_report(std::string_view text);
} // namespace xerces_core
