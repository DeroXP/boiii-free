// AMD FidelityFX Super Resolution 1.0, RCAS (robust contrast-adaptive
// sharpening), as a pixel shader: the upscaled picture (or the scene at the
// display size) into the back buffer. FSR 1 is MIT-licensed
// (deps/FidelityFX-FSR, THIRD_PARTY_NOTICES). Compiled by
// tools/shaders/build_shaders.py into fsr1_rcas_ps.h.

#define A_GPU 1
#define A_HLSL 1
#include "ffx_a.h"

Texture2D<float4> source : register(t0);

cbuffer rcas_constants : register(b0) {
  uint4 con;  // FsrRcasCon
  int4 last;  // the last pixel: width - 1, height - 1
};

#define FSR_RCAS_F 1
// Reads past the edge repeat the edge (a load out of bounds would be black).
AF4 FsrRcasLoadF(ASU2 p) {
  return source.Load(int3(clamp(int2(p), int2(0, 0), last.xy), 0));
}
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#include "ffx_fsr1.h"

float4 main(float4 position : SV_POSITION) : SV_TARGET {
  AF3 color;
  FsrRcasF(color.r, color.g, color.b, AU2(position.xy), con);
  return float4(color, 1.0);
}
