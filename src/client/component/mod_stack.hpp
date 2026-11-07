#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mod_stack {
std::vector<std::string> get(const std::string &main_mod);
std::vector<std::string> get();
std::string serialize();
void set(const std::string &list);
bool is_loaded();
// Whether the stack this list gives is the one loaded, without setting it
// (a join checks the host's list before it takes it).
bool is_loaded(const std::string &list);
// Reloads the stack at the next front-end frame, as ModStack.Reload does.
void request_reload();

// The custom map being loaded or played: the engine's active usermap's
// internal name (the name of its zones), set when its load starts; "" for
// stock maps. The out-of-memory report names the map by it, because the
// mapname dvar still names the level before while one loads.
std::string active_usermap();

// Map-first scripts (DESIGN.md 6.3 rule 6, xerces_map_scripts_first) must be
// the same on the host and every client. A host announces its own choice
// ("1" or "0") in getinfo and in the lobby content; a client joining another
// host follows what it announced ("" from a host that does not announce it:
// the engine's order), and a game this process starts itself uses its own.
std::string map_scripts_first_info();
void follow_host_map_scripts_first(std::string_view announced);
void use_own_map_scripts_first();

// Every stacked mod's copy of an entry script (like _clientids.gsc), renamed
// so each runs beside the others and beside the game's own copy.
std::vector<std::pair<std::string, std::string>> script_copies();

// Xerces (DESIGN.md 7.6, read only): the Workshop id of the mod (main or
// extra) or custom map whose copy of an asset the game uses now, for the
// menus' mod labels (data/ui_scripts/mod_labels). Nothing for the game's own
// copy, an asset not loaded, or a name the type has no asset for (the game
// hands out a stand-in for those). Ask for names the menus show, such as an
// item's image. type is the engine's XAssetType value.
std::optional<std::string> asset_owner(int32_t type, const std::string &name);
// A mod's or custom map's Workshop title; "" when neither is installed.
std::string workshop_title(const std::string &id);

// The game's own copy of an asset a mod or custom map replaced: the header of
// the highest copy from the game's zones, linked behind the one in use.
// nullptr when the copy in use is the game's own or the game has none. For
// the lobby Lua engine's fallback (ui_scripting.cpp, lobby_game_copies).
const void *game_copy(int32_t type, const std::string &name);

} // namespace mod_stack
