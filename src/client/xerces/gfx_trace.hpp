#pragma once

// The Direct3D 11 frame tracer (docs/DESIGN.md 13.3, the spike that finds the
// render passes an upscaler replaces). A developer tool: on only when the
// game starts with XERCES_GFX_TRACE=1, and then it detours d3d11.dll's
// device and context methods through their COM vtables (no game addresses).
//
// A capture writes one frame's draws, dispatches, copies and clears to
// <home>\bench\gfx-trace\frame-<n>.txt: for each, where in the game it was
// called from, the shaders, the render and depth targets, the textures and
// samplers it reads, and the viewport; with "cb" also the first 256 bytes of
// each pixel or compute shader constant buffer. The shaders it used go to
// shaders\<id>.<vs|ps|cs>.dxbc (the id is the DXBC checksum's first 8
// bytes), for fxc /dumpbin.
//
// Captures are asked for with a file: <dir>\arm holding "<frames> [cb]"
// (read twice a second, then deleted). <dir>\exec.cfg, when it appears, is
// run as console commands and deleted (render scale and anti-aliasing
// changes for a scripted spike).

#include <std_include.hpp>

#include <atomic>

namespace xerces_gfx_trace {
namespace detail {
extern std::atomic_bool on;
} // namespace detail

inline bool enabled() { return detail::on.load(std::memory_order_relaxed); }

// download_overlay.cpp's Present hook, before anything else it does: a frame
// ends at each Present of the game's swap chain.
void on_present(IDXGISwapChain *chain);
} // namespace xerces_gfx_trace
