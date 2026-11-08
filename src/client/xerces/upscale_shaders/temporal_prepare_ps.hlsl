// The temporal upscaler's inputs from what the game's SMAA T2x resolve reads
// (src/client/xerces/temporal.cpp), one pixel each at the render size:
//   0: colour, linear 0 to 1 (the game keeps it times 32768)
//   1: motion in render pixels, from this frame's position to the last
//      frame's (the game stores the other way, nonlinear, 16-bit SNORM)
//   2: depth, reversed (the game's own)
// Compiled by tools/shaders/build_shaders.py into temporal_prepare_ps.h.

Texture2D<float4> color : register(t0);
Texture2D<float2> velocity : register(t1);
Texture2D<float> depth : register(t2);

struct ps_out {
  float4 color : SV_TARGET0;
  float2 motion : SV_TARGET1;
  float depth : SV_TARGET2;
};

// The game's decode (its T2x resolve): |x| in [0, 1] to up to 40 pixels.
float2 decode_velocity(float2 x) {
  const float2 a = abs(x);
  const float2 pixels = saturate((a - 0.5) * 2.0) * 30.0 + min(a * 2.0, 1.0) * 10.0;
  return (x >= 0.0) ? pixels : -pixels;
}

ps_out main(float4 position : SV_POSITION) {
  const int3 p = int3(position.xy, 0);
  ps_out o;
  o.color = float4(color.Load(p).rgb * (1.0 / 32768.0), 1.0);
  // The game finds the last frame at uv - velocity: DLSS wants that offset.
  o.motion = -decode_velocity(velocity.Load(p));
  o.depth = depth.Load(p);
  return o;
}
