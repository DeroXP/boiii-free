#pragma once

// The upscaler's rules (docs/DESIGN.md 13.3, Phase A), kept here so they can
// be tested without the game; src/client/xerces/upscale.cpp applies them.
//
// Black Ops III renders the 3D scene at r_sceneResolutionMultiplier percent of
// the window and its final composite (signature r.final_composite) stretches
// it to the back buffer with a bilinear sampler while applying the display
// encoding (gamma and brightness), before the HUD is drawn at full size. With
// AMD FSR 1 on, the client runs that same composite shader at the scene's own
// size, upscales the result with EASU and sharpens it with RCAS into the back
// buffer, so the HUD stays as sharp as before.

#include <array>
#include <cstdint>
#include <string_view>

namespace xerces_core::upscale {
// The signature feature the composite detour belongs to (compat/
// signatures.json). A miss turns only the upscaler off: the game keeps its
// own bilinear stretch.
inline constexpr std::string_view feature = "upscale_spatial";

// The bytes the composite detour's jmp overwrites (the registry leaves them
// as wildcards): mov rax, rsp; and the first two bytes of mov [rax+20h], rbx.
inline constexpr std::array<std::uint8_t, 5> composite_entry = {
    0x48, 0x8B, 0xC4, 0x48, 0x89};

// xerces_upscaler's values.
enum class mode : int { off = 0, fsr1 = 1 };
inline constexpr int mode_max = 1;

// xerces_upscalerSharpness: 0 to 10 (tenths); 0 leaves RCAS out.
inline constexpr int sharpness_max = 10;
inline constexpr int sharpness_default = 8;

// What to run for one frame.
struct passes {
  bool active = false; // anything at all (else the game's own picture stays)
  bool easu = false;   // the scene is smaller than the back buffer
  bool rcas = false;   // sharpening is on
};

// The scene's size (the composite's source texture) and the back buffer's.
// Nothing runs when FSR 1 is off, either size is zero, or the scene is larger
// than the back buffer in either direction (EASU only enlarges; the game's own
// stretch then stays). With the scene as large as the back buffer only RCAS
// can run.
passes plan(mode chosen, int sharpness, std::uint32_t scene_width,
            std::uint32_t scene_height, std::uint32_t display_width,
            std::uint32_t display_height);

// RCAS's strength in stops (FSR 1: 0 is the strongest, each stop halves it):
// sharpness 10 is 0 stops, 0 would be 2.
float rcas_stops(int sharpness);

// FsrEasuCon from ffx_fsr1.h (FSR 1.0.2, MIT): con0..con3, 16 words, for an
// input image that fills its texture.
std::array<std::uint32_t, 16> easu_constants(float input_width,
                                             float input_height,
                                             float output_width,
                                             float output_height);

// FsrRcasCon from ffx_fsr1.h: con.x is the linear sharpness; con.y (the packed
// half-precision pair) only the FP16 shader reads, so it stays 0 here.
std::array<std::uint32_t, 4> rcas_constants(float stops);
} // namespace xerces_core::upscale
