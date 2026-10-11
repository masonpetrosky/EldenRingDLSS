// A stochastic replacement for the game's ScreenSpaceAO2_BilateralUpScale while Ray Reconstruction gets the raw ray
// traced AO. The game averages the 4 nearest half-size AO samples, weighted by depth similarity and distance; with raw
// noise that turns every half-size sample into a 2x2 block, which RR cannot tell from detail. This computes the same
// weights but picks one sample per pixel and frame at random, with probability weight / total: the same expected
// value, with noise that is independent per pixel and per frame. Inputs as the game's pass binds them: the full-size
// depth and the half-size AO (x = AO, yzw = linear depth packed as the game reads it).
// PSFull instead copies the AO our shadow ray generation shader traced at the render size (rt_raygen.hlsl), from the
// 4th channel of the shadow output's copy bound at t1; no upscale needed.
// SPDX-License-Identifier: MIT

Texture2D<float> g_depth : register(t0);
Texture2D<float4> g_src : register(t1);
SamplerState g_point : register(s0);

cbuffer Constants : register(b0) {
  float4 g_depth_param;   // cbSSAOParam g_vDepthComputeParam
  float4 g_screen;        // cbSSAOParam2 g_vScreenSize: half size, 1 / half size
  float2 g_percentage;    // cbPostProcessCommon g_dynamicScreenPercentage
  float2 g_inv_viewport;  // 1 / the pass's viewport size
  uint g_frame;
};

float4 VSMain(uint id : SV_VertexID) : SV_Position {
  const float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
}

// PCG hash (Jarzynski and Olano, 2020).
uint Hash(uint v) {
  const uint state = v * 747796405u + 2891336453u;
  const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}

float PSFull(float4 position : SV_Position) : SV_Target {
  return g_src.Load(int3(position.xy, 0)).w;
}

float PSMain(float4 position : SV_Position) : SV_Target {
  const float2 uv = position.xy * g_inv_viewport;  // the game's TEXCOORD
  const float depth = g_depth.SampleLevel(g_point, uv * g_percentage, 0.0f);
  const float inv_center = (g_depth_param.x - depth) / -g_depth_param.y;
  const float2 h = g_screen.zw * 0.5f;
  const float2 lo = (uv - h) * g_percentage, hi = (uv + h) * g_percentage;
  const float4 s[4] = {g_src.SampleLevel(g_point, float2(lo.x, lo.y), 0.0f),
                       g_src.SampleLevel(g_point, float2(hi.x, lo.y), 0.0f),
                       g_src.SampleLevel(g_point, float2(lo.x, hi.y), 0.0f),
                       g_src.SampleLevel(g_point, float2(hi.x, hi.y), 0.0f)};
  const float2 f = frac(g_screen.xy * uv);
  const float2 g = 1.0f - f;
  const float dist[4] = {g.x * g.x + g.y * g.y, f.x * f.x + g.y * g.y, g.x * g.x + f.y * f.y, f.x * f.x + f.y * f.y};
  float w[4];
  float total = 0.0f;
  [unroll] for (int i = 0; i < 4; ++i) {
    const float tap_depth = dot(s[i].yzw, float3(1.53787e-05f, 0.00392157f, 1.0f));
    w[i] = rsqrt(max(dist[i], 1e-6f)) / (abs(tap_depth * inv_center - 1.0f) + 0.01f);
    total += w[i];
  }
  const uint2 p = uint2(position.xy);
  const float r = (Hash(p.x ^ Hash(p.y ^ Hash(g_frame))) & 0xFFFFFF) / 16777216.0f * total;
  float sum = 0.0f;
  float ao = s[3].x;
  [unroll] for (int j = 0; j < 3; ++j) {
    sum += w[j];
    if (r < sum) {
      ao = s[j].x;
      break;
    }
  }
  return ao;
}
