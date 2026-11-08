// AMD FidelityFX Super Resolution 1.0, EASU (edge-adaptive spatial
// upsampling), as a pixel shader: the scene at its own size, already in the
// game's display encoding, to the display size. FSR 1 is MIT-licensed
// (deps/FidelityFX-FSR, THIRD_PARTY_NOTICES). Compiled by
// tools/shaders/build_shaders.py into fsr1_easu_ps.h.

#define A_GPU 1
#define A_HLSL 1
#include "ffx_a.h"

Texture2D<float4> source : register(t0);
SamplerState linear_clamp : register(s0);

cbuffer easu_constants : register(b0) {
  uint4 con0;
  uint4 con1;
  uint4 con2;
  uint4 con3;
};

#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return source.GatherRed(linear_clamp, p, int2(0, 0)); }
AF4 FsrEasuGF(AF2 p) { return source.GatherGreen(linear_clamp, p, int2(0, 0)); }
AF4 FsrEasuBF(AF2 p) { return source.GatherBlue(linear_clamp, p, int2(0, 0)); }
#include "ffx_fsr1.h"

float4 main(float4 position : SV_POSITION) : SV_TARGET {
  AF3 color;
  FsrEasuF(color, AU2(position.xy), con0, con1, con2, con3);
  return float4(color, 1.0);
}
