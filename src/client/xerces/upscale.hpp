#pragma once

// The upscaler (docs/DESIGN.md 13; the rules are in xerces_core/upscale.hpp):
// AMD FSR 1 in place of the game's own scene stretch (upscale.cpp, at the
// final composite) and NVIDIA DLSS or DLAA through the optional module
// (temporal.cpp, at the SMAA T2x resolve). Graphics > Video > Upscaler and
// Upscaler Sharpness (xerces_upscaler, xerces_upscalerSharpness).

#include <std_include.hpp>

#include <xerces_core/upscale.hpp>

#include <utility>

namespace xerces_upscale {
// download_overlay.cpp's Present hook: the first call keeps the game's device
// and immediate context, which the detours draw with; every call notes the
// back buffer's size.
void on_present(IDXGISwapChain *chain);

// For temporal.cpp (render thread unless said otherwise).
xerces_core::upscale::mode chosen_mode(); // any thread
int sharpness();                          // any thread
ID3D11DeviceContext *game_context();
std::pair<UINT, UINT> display_size(); // the last Present's back buffer
std::uint32_t present_count();        // Presents so far
} // namespace xerces_upscale

namespace xerces_upscale::temporal {
// The first Present: whether the DLSS module is there (xerces_dlss.dll next
// to the game client, an NVIDIA GPU), for the menus' xerces_dlssAvailable.
void on_first_present(IDXGISwapChain *chain);

// The composite detour: this frame's DLSS output at the back buffer's size,
// in the game's encoding, when DLSS upscaled this frame; else null.
ID3D11ShaderResourceView *display_output();
} // namespace xerces_upscale::temporal
