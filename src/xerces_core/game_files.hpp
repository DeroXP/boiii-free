#pragma once

// Two files in <game>\boiii_players\user that the client reads and upstream's
// launcher edited (DESIGN.md 7.3 parity table: Binds, Asset Pools):
//
// - binds.cfg: one "bind <key> \"<command>\"" line per custom bind; the client
//   runs them after its config (src/client/component/binds.cpp).
// - launcher_settings.json: "asset_limits_enabled" ("1" or "0") turns the
//   client's larger asset pools on or off (component/asset_limits.cpp); the
//   other keys (pool sizes "ap_*", friends_only) are kept as they are.
//
// Same rules as upstream's launcher.cpp, so files either one writes read the
// same in both.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xerces_core {
struct game_bind {
  std::string key;     // lower case, as upstream stores it
  std::string command; // without the quotes
};

// The checks for a bind the player adds, as upstream's saveBind makes them.
// A key: 1 to 32 characters, no spaces, quotes or ';'.
bool valid_bind_key(std::string_view key);
// A command: 1 to 512 characters, no quotes or line breaks (it is written
// inside quotes on one line).
bool valid_bind_command(std::string_view command);

// Lines that are not "bind <key> <command>" are skipped; a later bind of the
// same key replaces an earlier one; sorted by key.
std::vector<game_bind> parse_binds(std::string_view text);
// One line per entry. Entries are written as they are, not checked like a
// new bind: the game saves any bind the engine took (component/binds.cpp, no
// length limit) and upstream's launcher writes back every bind it read, so a
// bind the file already had is kept when another is added or removed. Only
// an entry that would not read back as itself is left out: an empty key or
// command, a key with a space or blanks around it, or a line break in either
// (none of which parse_binds gives).
std::string binds_to_text(const std::vector<game_bind> &binds);

// A running client owns both files: it reads them when it starts, and it
// writes binds.cfg from its own copy a few seconds after each bind change in
// the game (component/binds.cpp, save_binds), which would undo an edit made
// under it. Xerces sees a game it started itself, and one that announces
// itself in <home>\run, but a client started directly or by upstream's
// launcher does neither. So, as upstream's launcher does
// (is_game_process_running): any process named boiii.exe or BlackOps3.exe
// counts, and so does client_name (Xerces' game role) when given, whatever
// folder it is in, since a client's game folder is not visible from outside.
// Dedicated servers do not count; names in any case.
bool is_game_client(std::string_view exe_name, std::string_view client_name);
// The executable name of the first running client found; nullopt if none.
std::optional<std::string> running_game_client(std::string_view client_name);

// asset_limits_enabled as the client reads it: missing or "1" is on (unless
// disable_asset_pools is "1"), anything else off (parse_pool_settings in
// asset_limits.hpp, which also reads the pool sizes).
bool asset_pools_enabled(std::string_view launcher_settings_json);
// The file with asset_limits_enabled set and disable_asset_pools removed,
// keeping every other key; a missing or broken file becomes an object.
std::string set_asset_pools_enabled(std::string_view launcher_settings_json,
                                    bool enabled);
} // namespace xerces_core
