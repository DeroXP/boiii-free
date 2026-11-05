#pragma once

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
// Reloads the stack at the next front-end frame, as ModStack.Reload does.
void request_reload();

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

} // namespace mod_stack
