#pragma once

#include <std_include.hpp>

namespace gpu {
int get_preference();
void set_preference(int preference);
void check_device(ID3D11Device *device);
// The lobby's main menu is up (the Lua UI answers; not in a level). Main
// thread only.
bool on_main_menu();
} // namespace gpu
