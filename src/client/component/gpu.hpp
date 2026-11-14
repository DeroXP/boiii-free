#pragma once

#include <std_include.hpp>

namespace gpu {
int get_preference();
void set_preference(int preference);
void check_device(ID3D11Device *device);
// Present's result, from the Present detour (download_overlay.cpp) when it
// failed: a lost device is noted once, with Windows' reason, and logged
// (gpu.device_lost). Any thread.
void note_present_result(IDXGISwapChain *swap_chain, HRESULT result);
// The reports' line for it ("" while the device is fine): exception.cpp.
std::string device_lost_line();
// The lobby's main menu is up (the Lua UI answers; not in a level). Main
// thread only.
bool on_main_menu();
} // namespace gpu
