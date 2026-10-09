// Replacement pixel shader for the depth prepass of the game's grass (M[AMSN_V]_Grass2_GBufDpt and
// M[AMSN_V]_Vesitation_GBufDpt). The game's vertex shader stays, so the prepass writes exactly the game's depth and
// stencil; this shader runs the game's alpha test (the prepass shader's only job) and also writes the wind motion
// and its depth into the side targets that the TAA-time merge reads, so the grass needs no second velocity draw.
//
// The motion is rebuilt per pixel from what the game's vertex shader passes on: the bent position (POSITION1, through
// FC_MatrixInvViewProj, as the game's shader does for its distance fade), the instance (NORMAL.w), the blade's pivot
// (TEXCOORD5.xyz), its wind phase (TEXCOORD0.z) and its stiffness (TEXCOORD0.w). The current bend is inverted to the
// rest position, which is bent again with the previous frame's wind.
//
// Compiled with dxc (the game's shaders are DXIL, and a pipeline cannot mix DXIL and DXBC). The pixel stage of the
// game's root signature has no cbPerFrame and g_forceParam, so the replacement root signature adds them in space 1.
// ALPHA_TEST 1: Grass2, 2: Vesitation (see wind_common.hlsli).
// SPDX-License-Identifier: MIT

cbuffer cbTextureTilingScale : register(b1) {
  float4 g_Tiling;  // FC_TextureTilingScale[0]
};
cbuffer cbMatDynParam : register(b2) {
  float4 g_MatDyn[7];  // [0].w: FC_ColorSlotNo
};
cbuffer cbMtdParam : register(b3) {
  float4 g_Mtd[12];  // [1]: FC_AlphaRef; byte 144 = [9], 160 = [10]
};
cbuffer cbInstanceData : register(b4) {
  float4 g_Instance[4096];  // 13 float4s per instance: rows 0-2 instance matrix, row 7 forces
};
cbuffer cbModelParam : register(b7) {
  float4 g_Model[3];  // [2].x: FC_DitherEnable, .y: FC_DitherFadePatternSwitch
};
cbuffer cbSceneParam : register(b8) {
  float4 g_Scene[126];
};
cbuffer cbPerFrame : register(b0, space1) {
  float4 g_Frame[28];  // [27]: FC_GlobalTime, FC_TimeOfDay, FC_GlobalDt
};
Texture2D<float4> g_Material : register(t0);
Texture2D<float4> g_DitherTexture : register(t38);
SamplerState SS_DiffuseMap : register(s0);
SamplerState SS_WrapPoint : register(s6);

#define FORCE_PARAM_REGISTER t0, space1
#include "wind_common.hlsli"

// The game vertex shader's outputs, in its order (the signatures must line up row for row).
struct Input {
  float4 position : SV_Position;
  float4 clip : POSITION1;      // the bent position, clip space
  float4 normal : NORMAL;       // .w: instance data index
  float4 tangent : TANGENT;
  float4 texcoord0 : TEXCOORD0;  // .xy: UV, .z: wind phase (TEXCOORD1.x), .w: bend resistance (TEXCOORD2.z)
  float4 texcoord5 : TEXCOORD5;  // .xyz: pivot in instance space (TEXCOORD3)
};

struct Output {
  float2 velocity : SV_Target0;  // the game's encoding
  float depth : SV_Target1;      // this surface's depth, for the merge
};

Output PSMain(Input input) {
  const float4 h = float4(dot(g_Scene[kInvViewProj], input.clip), dot(g_Scene[kInvViewProj + 1], input.clip),
                          dot(g_Scene[kInvViewProj + 2], input.clip), dot(g_Scene[kInvViewProj + 3], input.clip));
  const float3 world = h.xyz / h.w;
  const uint instance = (uint)round(input.normal.w);
  if (AlphaDiscarded(input.texcoord0.xy, world, input.position.xy, instance)) {
    discard;
    return (Output)0;
  }

  const float4 r0 = g_Instance[instance * 13], r1 = g_Instance[instance * 13 + 1], r2 = g_Instance[instance * 13 + 2];
  const float4 q = float4(input.texcoord5.xyz, 1);
  const float3 pivot = float3(dot(r0, q), dot(r1, q), dot(r2, q));
  const float3 up = normalize(float3(r0.y, r1.y, r2.y));
  const float t = g_Frame[27].x + g_Mtd[10].PHASE_COMP * input.texcoord0.z;
  const float bend_scale = g_Mtd[10].BEND_COMP;
  const float weight = 1 - input.texcoord0.w;
  float3 disp_cur, disp_prev;
  ForceDisplacements(instance, pivot, float2(t, t - g_Frame[27].z), disp_cur, disp_prev);
  const float3 rest = Unbend(world, pivot, up, disp_cur, bend_scale, weight);
  const float3 prev_world = Bend(rest, pivot, up, disp_prev, bend_scale, weight);

  // Both ends through the forward matrices, so whatever FC_MatrixInvViewProj leaves out (jitter) cancels.
  Output o;
  o.velocity = EncodeMotion(Clip(world, kCameraPos, kViewProj), Clip(prev_world, kPrevCameraPos, kPrevViewProj));
  o.depth = input.position.z;
  return o;
}
