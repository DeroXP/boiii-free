#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace xerces_loader {
// The level loaded or loading ("" in the menus), from the mapname dvars, as
// valid UTF-8. Also used by the out-of-memory report (out_of_memory.cpp).
std::string current_map();

// Engine errors the player sees go into the game log as loader.engine.error
// with the map, the journal phase and the console's last lines; restarts and
// disconnects are left out. patches.cpp passes every Com_Error (source
// "com_error": a custom map whose scripts fail to link drops back to the
// menu with one) and every Sys_Error ("sys_error", which then ends the game
// through the fatal-error dialog, past the exit hook).
void engine_error(std::uint32_t code, std::string_view message,
                  std::string_view source = "com_error");
} // namespace xerces_loader
