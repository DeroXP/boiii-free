#pragma once

// The engine's out-of-memory exit, explained (docs/DESIGN.md 5.6, 5.7).
//
// When a Workshop custom map loads, the engine places blocks 5 and 6 of every
// loaded zone (fastfile) in one fixed 2 GiB pool, so a mod's zones and the
// map's share it. A large map with a large mod overflows it (2026-11-05: DS4C
// with zm_supermario64 or zm_backrooms). The engine's out-of-memory handler
// then calls exit(-1) on the loading thread, the C runtime destroys static
// objects while the main thread still draws, and the main thread crashes:
// the game just vanished. The game client replaces that handler
// (src/client/xerces/out_of_memory.cpp, feature "oom_report"); what it logs
// and tells the player is put together here, so it is tested without the
// game.

#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xerces_core {
// The zone the engine was loading: the name in the last
// "Loading fastfile 'zm_backrooms'" console line (lines oldest first, as
// xerces_runtime::console_last_lines gives them), as valid UTF-8 of at most
// 128 bytes. Empty when no such line is there.
std::string last_loading_zone(std::span<const std::string> console_lines);

// folder -> display name of each mod in a plan file's stack
// (<home>\config\plans\<id>.json, written by the launcher). Empty when the
// text is not a plan.
std::map<std::string, std::string>
read_plan_mod_names(std::string_view plan_json);

// The mods the engine has loaded, for people: fs_game first, then
// fs_extramods ("a,b", highest priority first). Each folder is shown by its
// name from `names` (matched without regard to ASCII letter case) or as the
// folder itself. Spaces around folders, empty entries and repeats are left
// out; each name is valid UTF-8 of at most 100 bytes.
std::vector<std::string>
loaded_mods(std::string_view fs_game, std::string_view fs_extramods,
            const std::map<std::string, std::string> &names = {});

// What the player is told, in plain words. `zone` may be empty (not known);
// `mods` are the names loaded_mods gives, empty when none are loaded.
struct out_of_memory_message {
  std::string title;
  std::string body;
  std::string summary; // one line, for the log
};
out_of_memory_message out_of_memory_text(std::string_view zone,
                                         std::span<const std::string> mods);
} // namespace xerces_core
