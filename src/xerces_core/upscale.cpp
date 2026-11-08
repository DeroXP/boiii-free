#include "upscale.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace xerces_core::upscale {
namespace {
std::uint32_t bits(float value) { return std::bit_cast<std::uint32_t>(value); }
} // namespace

passes plan(const mode chosen, const int sharpness,
            const std::uint32_t scene_width, const std::uint32_t scene_height,
            const std::uint32_t display_width,
            const std::uint32_t display_height) {
  passes out;
  if (chosen != mode::fsr1 || scene_width == 0 || scene_height == 0 ||
      display_width == 0 || display_height == 0 ||
      scene_width > display_width || scene_height > display_height) {
    return out;
  }
  out.easu = scene_width < display_width || scene_height < display_height;
  out.rcas = sharpness > 0;
  out.active = out.easu || out.rcas;
  return out;
}

float rcas_stops(const int sharpness) {
  const int clamped = std::clamp(sharpness, 0, sharpness_max);
  return 2.0f * static_cast<float>(sharpness_max - clamped) /
         static_cast<float>(sharpness_max);
}

// Ported from FsrEasuCon in deps/FidelityFX-FSR/ffx-fsr/ffx_fsr1.h (Copyright
// (c) 2021 Advanced Micro Devices, Inc., MIT), with the viewport the whole
// input texture.
std::array<std::uint32_t, 16> easu_constants(const float input_width,
                                             const float input_height,
                                             const float output_width,
                                             const float output_height) {
  std::array<std::uint32_t, 16> con{};
  // Output integer position to a pixel position in the viewport.
  con[0] = bits(input_width * (1.0f / output_width));
  con[1] = bits(input_height * (1.0f / output_height));
  con[2] = bits(0.5f * input_width * (1.0f / output_width) - 0.5f);
  con[3] = bits(0.5f * input_height * (1.0f / output_height) - 0.5f);
  // Viewport pixel position to normalized image space (upper left of 'F').
  con[4] = bits(1.0f / input_width);
  con[5] = bits(1.0f / input_height);
  // Centers of the gather4s, the first from the upper left of 'F'.
  con[6] = bits(1.0f * (1.0f / input_width));
  con[7] = bits(-1.0f * (1.0f / input_height));
  // These are from (0) instead of 'F'.
  con[8] = bits(-1.0f * (1.0f / input_width));
  con[9] = bits(2.0f * (1.0f / input_height));
  con[10] = bits(1.0f * (1.0f / input_width));
  con[11] = bits(2.0f * (1.0f / input_height));
  con[12] = bits(0.0f * (1.0f / input_width));
  con[13] = bits(4.0f * (1.0f / input_height));
  con[14] = 0;
  con[15] = 0;
  return con;
}

// Ported from FsrRcasCon in ffx_fsr1.h (as above).
std::array<std::uint32_t, 4> rcas_constants(const float stops) {
  return {bits(std::exp2(-stops)), 0, 0, 0};
}
} // namespace xerces_core::upscale
