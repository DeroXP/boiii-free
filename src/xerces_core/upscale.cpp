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

temporal_plan plan_temporal(const mode chosen, const std::uint32_t render_width,
                            const std::uint32_t render_height,
                            const std::uint32_t output_width,
                            const std::uint32_t output_height) {
  temporal_plan out;
  if (chosen != mode::dlss || render_width == 0 || render_height == 0 ||
      output_width == 0 || output_height == 0 || render_width > output_width ||
      render_height > output_height) {
    return out;
  }
  out.active = true;
  out.dlaa = render_width == output_width && render_height == output_height;
  return out;
}

float halton(std::uint32_t index, const std::uint32_t base) {
  float fraction = 1.0f;
  float result = 0.0f;
  while (index > 0) {
    fraction /= static_cast<float>(base);
    result += fraction * static_cast<float>(index % base);
    index /= base;
  }
  return result;
}

std::uint32_t jitter_phases(const std::uint32_t render_width,
                            const std::uint32_t render_height,
                            const std::uint32_t output_width,
                            const std::uint32_t output_height) {
  if (render_width == 0 || render_height == 0) {
    return 8;
  }
  const double ratio = (static_cast<double>(output_width) * output_height) /
                       (static_cast<double>(render_width) * render_height);
  const auto phases = static_cast<std::uint32_t>(std::ceil(8.0 * ratio));
  return std::clamp<std::uint32_t>(phases, 8, 64);
}

jitter temporal_jitter(const std::uint32_t frame, const std::uint32_t phases) {
  const std::uint32_t index = 1 + frame % std::max<std::uint32_t>(phases, 1);
  return {halton(index, 2) - 0.5f, halton(index, 3) - 0.5f};
}

// Ported from FsrRcasCon in ffx_fsr1.h (as above).
std::array<std::uint32_t, 4> rcas_constants(const float stops) {
  return {bits(std::exp2(-stops)), 0, 0, 0};
}
} // namespace xerces_core::upscale
