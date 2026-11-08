#pragma once

// AMD FSR 1 in place of the game's own scene stretch (docs/DESIGN.md 13.3;
// the rules are in xerces_core/upscale.hpp). Graphics > Video > Upscaler and
// Upscaler Sharpness (xerces_upscaler, xerces_upscalerSharpness).

#include <std_include.hpp>

namespace xerces_upscale {
// download_overlay.cpp's Present hook: the first call keeps the game's device
// and immediate context, which the composite detour draws with.
void on_present(IDXGISwapChain *chain);
} // namespace xerces_upscale
