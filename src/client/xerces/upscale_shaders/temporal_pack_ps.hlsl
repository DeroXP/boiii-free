// The temporal upscaler's output back in the game's encoding (linear times
// 32768), one pixel each, for the game's own passes after its SMAA T2x resolve
// (src/client/xerces/temporal.cpp). Compiled by tools/shaders/build_shaders.py
// into temporal_pack_ps.h.

Texture2D<float4> source : register(t0);

float4 main(float4 position : SV_POSITION) : SV_TARGET {
  const float3 c = max(source.Load(int3(position.xy, 0)).rgb, 0.0);
  return float4(c * 32768.0, 1.0);
}
