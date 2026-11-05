#pragma once

// The engine's out-of-memory exit, explained (docs/DESIGN.md 5.6, 5.7).
//
// When a Workshop custom map loads, the engine places blocks 5 and 6 of every
// loaded zone (fastfile) in one fixed 2 GiB pool, so a mod's zones and the
// map's share it. A large map with a large mod overflows it (2026-11-05: DS4C
// with zm_supermario64 or zm_backrooms). The engine's out-of-memory handler
// then calls exit(-1) on the loading thread, the C runtime destroys static
// objects while the main thread still draws, and the main thread crashes:
// the game just vanished. The same handler also ends the engine's allocation
// wrappers when the C runtime's malloc fails. The game client replaces that
// handler (src/client/xerces/out_of_memory.cpp, feature "oom_report"); what
// it logs and tells the player is put together here, so it is tested without
// the game.

#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xerces_core {
// The zone the engine is still loading: the name in the last
// "Loading fastfile 'zm_backrooms'" console line that no later
// "FastFileLoad: Setting zm_backrooms to XZONE_LOADED" line marks loaded
// (lines oldest first, as xerces_runtime::console_last_lines gives them), as
// valid UTF-8 of at most 128 bytes. Empty when no zone is loading as far as
// the lines show (in a match, say).
std::string last_loading_zone(std::span<const std::string> console_lines);

// folder -> display name of each mod in a plan file's stack
// (<home>\config\plans\<id>.json, written by the launcher). Empty when the
// text is not a plan.
std::map<std::string, std::string>
read_plan_mod_names(std::string_view plan_json);

// The mods the engine has loaded, for people: fs_game first, then
// fs_extramods ("a,b", highest priority first). As mod_stack::get decides,
// none when fs_game is empty or "usermaps" (the engine's pseudo-mod for a
// custom map played without a mod; any ASCII letter case), whatever
// fs_extramods says. Each folder is shown by its name from `names` (matched
// without regard to ASCII letter case) or as the folder itself. Spaces around
// folders, empty entries and repeats are left out; each name is valid UTF-8
// of at most 100 bytes.
std::vector<std::string>
loaded_mods(std::string_view fs_game, std::string_view fs_extramods,
            const std::map<std::string, std::string> &names = {});

// The level the report names: the custom map the engine has active (its
// internal name; the engine sets it when that map's load starts) when there
// is one, else the mapname dvar. While a level loads, mapname still names the
// level before it (core_frontend from the menus, in every out-of-memory run
// on 2026-11-05), so for a stock map that is still loading the zone is the
// better guide. Valid UTF-8 of at most 64 bytes; spaces around are left out.
std::string out_of_memory_map(std::string_view active_usermap,
                              std::string_view mapname);

// What the player is told, in plain words. `zone` is what last_loading_zone
// gives: with one, the text names it and the mods and says to remove a mod
// (or, with none, to play another map); without one (no zone is loading, or
// it is not known), it only says the game ran out of memory. `mods` are the
// names loaded_mods gives, empty when none are loaded.
struct out_of_memory_message {
  std::string title;
  std::string body;
  std::string summary; // one line, for the log
};
out_of_memory_message out_of_memory_text(std::string_view zone,
                                         std::span<const std::string> mods);
} // namespace xerces_core
