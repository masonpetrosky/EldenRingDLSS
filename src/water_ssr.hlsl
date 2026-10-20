// The water reflections' depth inputs at the add-on's size (water_ssr.hpp): the linear depth the reflection rays march
// through (GXSsrConvertDepth's g_ConvertedDepthTexture) and the depth buffer the water draws test against (the engine's
// half-size depth from DepthScale2x2OutputDepth), both made from the full-size depth in one pass.
// SPDX-License-Identifier: MIT

Texture2D<float> g_Depth : register(t0);  // the full-size depth, the scene in its top-left share

cbuffer Params : register(b0) {
  float2 g_Scale;        // full-size depth texels per target texel
  uint2 g_Share;         // the scene's share of the target, in texels; beyond it the far border
  uint2 g_SourceShare;   // the scene's share of the full-size depth, in texels
  float2 g_DepthParam;   // cbPerScene.g_vDepthComputeParam .x and .z
};

struct Output {
  float linear_depth : SV_Target;
  float depth : SV_Depth;
};

float4 VSMain(uint id : SV_VertexID) : SV_Position {
  const float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

Output PSMain(float4 position : SV_Position) {
  const uint2 p = uint2(position.xy);
  // Reversed Z: 0 is the far plane. Outside the share this is GXSsrFillDepth's border, the far plane for the
  // bilinear taps at the edge.
  float d = 0;
  if (all(p < g_Share)) {
    // The full-size texels whose centers fall in this texel ([p, p + 1) * scale, at most 2 per axis), the nearest
    // wins, as in the engine's 2x2 downscale.
    const int2 first = min(int2(ceil(p * g_Scale - 0.5)), int2(g_SourceShare) - 1);
    const int2 last = min(int2(ceil((p + 1) * g_Scale - 0.5)) - 1, int2(g_SourceShare) - 1);
    d = g_Depth.Load(int3(first, 0));
    if (last.x > first.x) d = max(d, g_Depth.Load(int3(last.x, first.y, 0)));
    if (last.y > first.y) {
      d = max(d, g_Depth.Load(int3(first.x, last.y, 0)));
      if (last.x > first.x) d = max(d, g_Depth.Load(int3(last, 0)));
    }
  }
  Output o;
  o.linear_depth = -(g_DepthParam.y * (g_DepthParam.x - d));  // GXSsrConvertDepth's formula
  o.depth = d;
  return o;
}
