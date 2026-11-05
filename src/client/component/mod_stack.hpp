#pragma once

#include <string>
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

// Every stacked mod's copy of an entry script (like _clientids.gsc), renamed
// so each runs beside the others and beside the game's own copy.
std::vector<std::pair<std::string, std::string>> script_copies();

} // namespace mod_stack
