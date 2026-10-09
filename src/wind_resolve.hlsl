// Writes motion vectors for wind-animated vegetation into the TAA's motion texture. The vertices come from the game's
// own vertex shaders, streamed out twice per draw: once with the previous frame's camera and wind time (g_Prev) and
// once with the current ones (g_Cur), in the same order. Fragments only write where their depth matches the scene
// depth, i.e. where this vegetation is what the camera actually sees (alpha-tested holes keep the motion behind them).
// SPDX-License-Identifier: MIT

StructuredBuffer<float4> g_Prev : register(t0);
StructuredBuffer<float4> g_Cur : register(t1);
ByteAddressBuffer g_Filled : register(t2);  // bytes streamed out: previous at 0, current at 16
Texture2D<float> g_Depth : register(t3);

cbuffer Params : register(b0) {
  float2 g_Scale;           // NDC delta to the game's encoding: (0.5 * width / height, -0.5) * FC_VelocityEncodeParam.x
  float g_Bias;             // FC_VelocityEncodeParam.y
  float g_DepthTolerance;   // relative
  uint g_Capacity;          // vertices per stream buffer
};

struct Vertex {
  float4 position : SV_Position;
  float4 prev : TEXCOORD0;
  float4 cur : TEXCOORD1;
};

Vertex VSMain(uint id : SV_VertexID) {
  Vertex v;
  const uint count = min(min(g_Filled.Load(0), g_Filled.Load(16)) / 16, g_Capacity);
  if (id >= count) {
    v.position = float4(2, 2, 2, 1);  // outside the view volume, so the triangle is clipped away
    v.prev = v.cur = 0;
    return v;
  }
  v.prev = g_Prev[id];
  v.cur = g_Cur[id];
  v.position = v.cur;
  return v;
}

float2 PSMain(Vertex v) : SV_Target {
  const float scene = g_Depth.Load(int3(v.position.xy, 0));
  if (abs(scene - v.position.z) > g_DepthTolerance * max(scene, 1e-7)) discard;
  const float2 delta = v.cur.xy / v.cur.w - v.prev.xy / v.prev.w;
  return delta * g_Scale + g_Bias;
}
