// Copies the wind velocity drawn during the G-buffer pass (wind_port.hlsl) into the TAA's motion texture wherever
// the surface it belongs to is still the visible one, i.e. where its recorded depth matches the final scene depth.
// Anything drawn in front of the vegetation later, and the background seen through alpha-tested holes, keeps the
// game's own motion.
// SPDX-License-Identifier: MIT

Texture2D<float2> g_WindVelocity : register(t0);
Texture2D<float> g_WindDepth : register(t1);   // 0 where no vegetation was drawn
Texture2D<float> g_SceneDepth : register(t2);

cbuffer Params : register(b0) {
  float g_DepthTolerance;  // relative
  uint g_Debug;            // 1: mark merged pixels, 2: mark every pixel the vegetation pass drew
};

float4 VSMain(uint id : SV_VertexID) : SV_Position {
  const float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float2 PSMain(float4 position : SV_Position) : SV_Target {
  const int3 p = int3(position.xy, 0);
  const float wind = g_WindDepth.Load(p);
  const float scene = g_SceneDepth.Load(p);
  if (g_Debug == 2 && wind > 0) return float2(1, 1);
  if (wind <= 0 || abs(wind - scene) > g_DepthTolerance * scene) discard;
  return g_Debug == 1 ? float2(1, 1) : g_WindVelocity.Load(p);
}
