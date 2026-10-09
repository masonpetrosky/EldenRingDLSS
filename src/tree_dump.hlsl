// Development dump of SpeedTree wind (dev remote "treedump"): for every tree of one SpeedTree draw, its world position
// (cbInstanceData, pixel stage) and its wind entry (cbSpeedTreeWind, 17 float4s). Both are indexed like the game's
// vertex shader does: g_InstanceIndexBuffer[VC_InstanceIdxOffset + instance]. Output per tree: 18 float4s, the first
// holding the position and the index.
// SPDX-License-Identifier: MIT

cbuffer Wind : register(b0) {
  float4 g_Wind[4096];  // sSpeedTreeWind per tree, 17 float4s each
};
cbuffer Instances : register(b1) {
  float4 g_Instance[4096];  // 13 float4s per tree: float4x3 mWorld (translation in .w of the first three), ...
};
cbuffer IdxParam : register(b2) {
  int g_IdxOffset;  // VC_InstanceIdxOffset
};
cbuffer Args : register(b3) {
  uint g_Count;      // instances of the draw
  uint g_OutOffset;  // in float4s
};
ByteAddressBuffer g_IndexBuffer : register(t0);  // g_InstanceIndexBuffer
RWByteAddressBuffer g_Out : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= g_Count) return;
  const uint idx = g_IndexBuffer.Load((uint)(g_IdxOffset + (int)id.x) << 2);
  const uint base = (g_OutOffset + id.x * 18) * 16;
  const float4 position = float4(g_Instance[idx * 13].w, g_Instance[idx * 13 + 1].w, g_Instance[idx * 13 + 2].w,
                                 asfloat(idx));
  g_Out.Store4(base, asuint(position));
  for (uint k = 0; k < 17; ++k) g_Out.Store4(base + 16 + k * 16, asuint(g_Wind[idx * 17 + k]));
}
