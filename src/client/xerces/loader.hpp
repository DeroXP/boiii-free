#pragma once

#include <cstdint>
#include <string_view>

namespace xerces_loader {
// patches.cpp's Com_Error hook passes every engine error here. Errors a player
// sees (a custom map whose scripts fail to link drops back to the menu with
// one) go into the game log as loader.engine.error with the map, the journal
// phase and the console's last lines; restarts and disconnects are left out.
void engine_error(std::uint32_t code, std::string_view message);
} // namespace xerces_loader
