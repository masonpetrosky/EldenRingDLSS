// DLSS Ray Reconstruction guide buffers from Elden Ring's G-buffer, drawn at the sun light pass
// (GXLightAcc_DDirLightShadowed1), which decodes it like this (docs/raytracing.md): GBuffer0.xyz = world normal * 0.5 +
// 0.5; GBuffer1 = metalness, smoothness, wrap lighting; GBuffer2.rgb = base color (sRGB, read through an sRGB view),
// .a = light group. The game's GGX roughness is (1 - 0.95 sqrt(smoothness))^2, so the perceptual ("linear")
// roughness RR wants is 1 - 0.95 sqrt(smoothness). The view direction comes from the light pass's own constants
// (cbLightAccParam: g_InvProjMatrix, g_CameraMatrix), as the game computes it. Sky pixels (reverse-Z depth 0) get
// NVIDIA's defaults. Drawn with the light pass's viewport (the render size).
// SPDX-License-Identifier: MIT

Texture2D<float4> g_gbuffer0 : register(t0);
Texture2D<float4> g_gbuffer1 : register(t1);
Texture2D<float4> g_gbuffer2 : register(t2);
Texture2D<float> g_depth : register(t3);

cbuffer Constants : register(b0) {
  float4 g_inv_proj[4];  // rows: view position = (dot(row_i, (ndc, depth, 1)))_i, divided by w
  float4 g_camera[3];    // rows: world direction = dot(row_i.xyz, view direction)
  float2 g_size;         // viewport (render) size
  float2 g_inv_size;
};

// NVIDIA, DLSS-RR Integration Guide, appendix ([Ray Tracing Gems, chapter 32]).
float3 EnvBRDFApprox2(float3 specular_color, float alpha, float nov) {
  nov = abs(nov);
  float4 x = float4(1.0f, nov, nov * nov, nov * nov * nov);
  float4 y = float4(1.0f, alpha, alpha * alpha, alpha * alpha * alpha);
  float2x2 m1 = float2x2(0.99044f, -1.28514f, 1.29678f, -0.755907f);
  float3x3 m2 = float3x3(1.0f, 2.92338f, 59.4188f, 20.3225f, -27.0302f, 222.592f, 121.563f, 626.13f, 316.627f);
  float2x2 m3 = float2x2(0.0365463f, 3.32707f, 9.0632f, -9.04756f);
  float3x3 m4 = float3x3(1.0f, 3.59685f, -1.36772f, 9.04401f, -16.3174f, 9.22949f, 5.56589f, 19.7886f, -20.2123f);
  float bias = dot(mul(m1, x.xy), y.xy) * rcp(dot(mul(m2, x.xyw), y.xyw));
  float scale = dot(mul(m3, x.xy), y.xy) * rcp(dot(mul(m4, x.xzw), y.xyw));
  bias *= saturate(specular_color.g * 50.0f);  // NVIDIA's hack for a specular reflectance of 0
  return mad(specular_color, max(0.0f, scale), max(0.0f, bias));
}

float4 VSMain(uint id : SV_VertexID) : SV_Position {
  const float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
}

struct Guides {
  float4 normal_roughness : SV_Target0;
  float4 diffuse_albedo : SV_Target1;
  float4 specular_albedo : SV_Target2;
};

Guides PSMain(float4 position : SV_Position) {
  const int2 p = int2(position.xy);
  Guides o;
  const float depth = g_depth.Load(int3(p, 0));
  if (depth <= 0.0f) {
    o.normal_roughness = 0.0f;
    o.diffuse_albedo = float4(0.5f, 0.5f, 0.5f, 1.0f);
    o.specular_albedo = float4(0.0f, 0.0f, 0.0f, 1.0f);
    return o;
  }
  const float3 n = normalize(g_gbuffer0.Load(int3(p, 0)).xyz * 2.0f - 1.0f);
  const float4 g1 = g_gbuffer1.Load(int3(p, 0));
  const float3 base = g_gbuffer2.Load(int3(p, 0)).rgb;
  const float metalness = saturate(g1.x);
  const float roughness = 1.0f - 0.95f * sqrt(saturate(g1.y));
  const float2 ndc = float2(position.x * g_inv_size.x * 2.0f - 1.0f, 1.0f - position.y * g_inv_size.y * 2.0f);
  const float4 h = float4(ndc, depth, 1.0f);
  const float4 view = float4(dot(g_inv_proj[0], h), dot(g_inv_proj[1], h), dot(g_inv_proj[2], h), dot(g_inv_proj[3], h));
  const float3 v = view.xyz / view.w;
  const float3 toward_camera =
      -normalize(float3(dot(g_camera[0].xyz, v), dot(g_camera[1].xyz, v), dot(g_camera[2].xyz, v)));
  const float3 f0 = lerp(0.04f, base, metalness);
  o.normal_roughness = float4(n, roughness);
  o.diffuse_albedo = float4(base * (1.0f - metalness), 1.0f);
  o.specular_albedo = float4(EnvBRDFApprox2(f0, roughness * roughness, dot(n, toward_camera)), 1.0f);
  return o;
}
