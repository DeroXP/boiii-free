#pragma once

// The Mods screen's headless half (docs/DESIGN.md 7.3, 8.4): editing a mode's
// load order by hand, undoing those edits, matching the search box, and
// pinning the planner's diagnostics to the mods they name. No UI here, so the
// rules are unit tested (src/xerces_tests/test_mods.cpp).

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "diagnostics.hpp"
#include "launcher_config.hpp"
#include "manifest.hpp"
#include "planner.hpp"

namespace xerces_core {
// ------------------------------------------------------------- load order
// Everything a hand edit can change for one mode: the profile and the
// planner's saved positions (which are shared by every mode).
struct mods_snapshot {
  mode_profile profile;
  std::map<std::string, std::int64_t> saved_positions;
};

bool mods_same(const mode_profile &a, const mode_profile &b);
bool mods_same(const mods_snapshot &a, const mods_snapshot &b);

mods_snapshot mods_take(const launcher_config &config, const std::string &mode);
void mods_put(launcher_config &config, const std::string &mode,
              const mods_snapshot &snapshot);

// Turns a mod on or off. A mod switched on goes to the bottom of the order:
// the lowest priority, so the main mod (the top one, fs_game) and everything
// the player already ordered stay where they were. False if nothing changed.
bool mods_set_enabled(mode_profile &profile, const std::string &id, bool on);
// Moves the mod at `from` to `to` (both indexes into profile.mods; the mod
// takes the target's place). False if nothing changed.
bool mods_move(mode_profile &profile, std::size_t from, std::size_t to);
// Moves a mod up (delta < 0, towards the top) or down by |delta| places,
// stopping at the ends. False if it was already there or is not enabled.
bool mods_move_by(mode_profile &profile, const std::string &id, int delta);
// The map for the mode; nullopt for none.
bool mods_set_map(mode_profile &profile, const std::optional<std::string> &map);
// Drops one pinned version (a fix the planner offers).
bool mods_unpin(mode_profile &profile, const std::string &id);

// After an edit by hand the profile no longer matches a pack: its pins, name
// and id go, and the saved positions follow the new order so the planner's
// tie-break agrees with what the player sees. Returns the pack's name when
// one was detached, else an empty string.
std::string mods_hand_edit(launcher_config &config, const std::string &mode);

// A short history of hand edits for Ctrl+Z. Each entry keeps the state
// before and after the edit; undo only restores "before" while the profile
// still looks like "after", so it never throws away a change made elsewhere
// (Home, Packs) since.
class mods_history {
public:
  explicit mods_history(std::size_t limit = 24) : limit_(limit) {}

  void record(const std::string &mode, mods_snapshot before,
              mods_snapshot after);

  enum class undo_result { done, nothing, stale };
  // Undoes the newest edit for this mode. stale: the profile changed
  // elsewhere since, so that mode's history is dropped and nothing changes.
  undo_result undo(launcher_config &config, const std::string &mode);
  std::size_t size(const std::string &mode) const;

private:
  struct entry {
    std::string mode;
    mods_snapshot before;
    mods_snapshot after;
  };
  std::size_t limit_;
  std::deque<entry> entries_;
};

// ----------------------------------------------------------------- search
// A Workshop title without the game's colour codes ("^3PZM^7 v2" -> "PZM
// v2"): ^ and a digit. Other carets stay ("needs ^1.0" is a version range,
// so this is only for names).
std::string mods_plain_name(std::string_view name);

// Case-insensitive: every word of the query must appear in the name (with or
// without colour codes), the id, the folder or the Workshop id.
bool mods_matches(const installed_content &item, std::string_view query);

// ------------------------------------------------------------ diagnostics
struct mods_note {
  severity level = severity::error;
  std::string code;
  std::string message;
};

struct mods_notes {
  std::map<std::string, std::vector<mods_note>> by_id;
  // Plan-wide notes that name no mod (the zone budget, ...).
  std::vector<mods_note> general;
  bool has_error(const std::string &id) const;
  bool has_warning(const std::string &id) const;
};

// Which mods a plan's diagnostics are about. The planner writes ids in
// quotes and mods by their display names, so both are looked for, longest
// name first (so "Zombies+" is not also counted as "Zombies").
// names: (id, display name) for every mod the screen shows.
mods_notes
mods_attribute(const plan &resolved,
               const std::vector<std::pair<std::string, std::string>> &names);

// The catalog scan's diagnostics about one folder (its manifest's errors, a
// broken workshop.json).
std::vector<mods_note> mods_folder_notes(const diagnostics &diags,
                                         const std::string &folder);

// ------------------------------------------------------------------ text
// "sha256:0123456789abcdef..." -> "0123456789ab".
std::string mods_short_hash(std::string_view hash);
// One entry per declared capability: a fixed English label (the screen
// translates it) and the hosts or paths it names, if any.
struct mods_capability {
  std::string label;
  std::string detail;
};
std::vector<mods_capability> mods_capabilities(const capabilities &caps);
} // namespace xerces_core
