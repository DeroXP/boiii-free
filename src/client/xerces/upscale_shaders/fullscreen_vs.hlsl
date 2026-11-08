// The upscaler's full-screen triangle (src/client/xerces/upscale.cpp). Its
// output matches the game's own full-screen passes (SV_POSITION in o0,
// TEXCOORD0 in o1.xy), so the game's final composite pixel shader can run on
// it: that pass then draws the scene 1:1 at the scene's own size.
// Compiled by tools/shaders/build_shaders.py into fullscreen_vs.h.

struct vs_out {
  float4 position : SV_POSITION;
  float2 uv : TEXCOORD0;
};

vs_out main(uint id : SV_VertexID) {
  vs_out o;
  // (0,0), (2,0), (0,2): one triangle that covers the target.
  const float2 uv = float2((id << 1) & 2, id & 2);
  o.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  o.uv = uv;
  return o;
}
