#pragma once

// xerces_launch_dvars (launch_dvars.cpp): the launcher's settings are set
// again over config.cfg for this session only, and kept out of the
// archived dvars the client saves back into it (xerces_core/session_dvars).
// component/dvars.cpp calls these two; they are its only Xerces lines.

#include <optional>
#include <string>

namespace game {
union EngineDependentDvar;
}

namespace xerces_launch_dvars {
// Dvar_SetVariant set an archived dvar after config.cfg had run: a value
// other than the launcher's is the player's own from then on.
void archive_set(game::EngineDependentDvar dvar);

// What write_archive_dvars saves for an archived dvar running at running:
// running itself, config.cfg's value for a launcher setting, or nullopt to
// leave the line out (config.cfg had none, so the game's default stays).
std::optional<std::string> value_to_save(const char *name, const char *running);
} // namespace xerces_launch_dvars
