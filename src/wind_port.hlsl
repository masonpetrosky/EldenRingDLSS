// Motion vectors for the game's force-driven vegetation (M[AMSN_V]_Grass2, _Vesitation, _Grass2_LOD1): a port of the
// position math of their G-buffer vertex shaders (decompiled from M[AMSN_V]_Grass2_Gbuf and _Grass2_LOD1_Gbuf),
// evaluated for the current and the previous frame in one pass. Drawn with the game's root signature right after the
// game's own draw, into a velocity target (the game's motion encoding) and a depth target that the TAA-time merge
// compares with the final depth. The force, bend and alpha-test math is shared with wind_prepass.hlsl
// (wind_common.hlsli).
//
// LOD1 builds the LOD1 variant: pivot in TEXCOORD2, blades shrinking towards the pivot with camera distance
// (byte 160: .x range, .y start, .z keep, .w phase scale), bend scale at byte 176.x, bend weighted by the height above
// the pivot.
//
// ALPHA_TEST ports the alpha test of the materials' depth-prepass pixel shaders (M[AMSN_V]_Grass2_GBufDpt and
// _Vesitation_GBufDpt): texture alpha, camera-distance fades with an 8x8 dither, per-instance LOD dither and
// FC_AlphaRef. 1: Grass2 (one far fade, cbMtdParam byte 144), 2: Vesitation (plus a near fade, byte 160), 3: the
// Grass2_LOD1 G-buffer shader (FC_CameraFadeParam near/far fade). Without it, transparent parts of blade cards would
// record their depth over the blades behind them.
//
// By default both poses are evaluated together (FusedPoses): force loads, distances, falloffs and the blade's frame are
// shared and only the time-dependent terms run twice. Development variants for cost measurements: UNFUSED evaluates
// the two poses separately (the first version), STATIC skips the wind entirely.
// SPDX-License-Identifier: MIT

cbuffer cbMtdParam : register(b3) {
  float4 g_Mtd[12];  // byte 160 = [10], 176 = [11]
};
cbuffer cbInstanceData : register(b4) {
  // 13 float4s per instance: rows 0-2 instance matrix, row 4.x object matrix base, row 7 forces.
  float4 g_Instance[4096];
};
cbuffer cbObjMatrix : register(b5) {
  float4 g_ObjMatrix[4096];  // 3 rows per matrix
};
cbuffer cbSceneParam : register(b8) {
  float4 g_Scene[126];
};
cbuffer cbPerFrame : register(b9) {
  float4 g_Frame[28];  // [27]: FC_GlobalTime, FC_TimeOfDay, FC_GlobalDt
};
cbuffer cbInstanceIdxParam : register(b10) {
  int g_InstanceIdxOffset;
};
cbuffer cbTexScroll : register(b11) {
  float4 g_TexScroll;  // VC_TexScroll_0
};
// Pixel stage.
cbuffer cbTextureTilingScale : register(b1) {
  float4 g_Tiling;  // FC_TextureTilingScale[0]
};
cbuffer cbMatDynParam : register(b2) {
  float4 g_MatDyn[7];  // [0].w: FC_ColorSlotNo; [5]: FC_CameraFadeParam gradients; [6].x IsEnable, .y MiddleAlpha
};
cbuffer cbModelParam : register(b7) {
  float4 g_Model[3];  // [2].x: FC_DitherEnable, .y: FC_DitherFadePatternSwitch
};
Texture2D<float4> g_Material : register(t0);
Texture2D<float4> g_DitherTexture : register(t38);
SamplerState SS_DiffuseMap : register(s0);
SamplerState SS_WrapPoint : register(s6);
ByteAddressBuffer g_InstanceIndexBuffer : register(t25);

#define FORCE_PARAM_REGISTER t26
#include "wind_common.hlsli"

struct Input {
  float3 position : POSITION;
  uint4 normal : NORMAL;       // .w: object matrix offset
  int2 texcoord0 : TEXCOORD0;  // UV in 1/2048 units
  float4 texcoord1 : TEXCOORD1;  // .x: wind phase
  float4 texcoord2 : TEXCOORD2;  // .z: bend resistance (1 = rigid)
#ifndef LOD1
  float4 texcoord3 : TEXCOORD3;  // xyz: pivot in instance space
#endif
  uint instance : SV_InstanceID;
};

struct Vertex {
  float4 position : SV_Position;
  float4 cur : TEXCOORD0;
  float4 prev : TEXCOORD1;
  nointerpolation float3 encode : TEXCOORD2;  // NDC delta scale (x, y) and bias of the game's motion encoding
  float2 uv : TEXCOORD3;
  float3 world : TEXCOORD4;
  nointerpolation uint instance : TEXCOORD5;  // the instance's data index (after g_InstanceIndexBuffer)
};

// Summed displacement of all forces acting on this instance at wind time `t` (the UNFUSED variant's single pose).
float3 ForceDisplacement(uint instance, float3 pivot, float t) {
  float3 acc, unused;
  ForceDisplacements(instance, pivot, float2(t, t), acc, unused);
  return acc;
}

uint IndirectInstance(Input input) {
  return g_InstanceIndexBuffer.Load((uint)(g_InstanceIdxOffset + (int)input.instance) << 2);
}

// World position of the vertex at wind time `t`, seen from `camera` (LOD1 shrinks distant blades).
float3 WorldPosition(Input input, float t, float3 camera) {
  const uint inst = input.instance;
  const uint indirect = IndirectInstance(input);
  const int base = (int)(asint(g_Instance[indirect * 13 + 4].x) + (int)input.normal.w) * 3;
  const float4 p = float4(input.position, 1);
  const float3 world = float3(dot(g_ObjMatrix[base], p), dot(g_ObjMatrix[base + 1], p), dot(g_ObjMatrix[base + 2], p));
  const float4 r0 = g_Instance[inst * 13], r1 = g_Instance[inst * 13 + 1], r2 = g_Instance[inst * 13 + 2];
#ifdef LOD1
  const float4 q = float4(input.texcoord2.xyz, 1);
#else
  const float4 q = float4(input.texcoord3.xyz, 1);
#endif
  const float3 pivot = float3(dot(r0, q), dot(r1, q), dot(r2, q));
  const float3 up = normalize(float3(r0.y, r1.y, r2.y));
#ifdef LOD1
  const float4 m = g_Mtd[10];
  const float fade = saturate((length(camera - world) - m.y) * (1 / m.x));
  const float3 base_pos = world + fade * (pivot - world) * (1 - m.z);
  const float3 disp = ForceDisplacement(inst, pivot, t + m.w * input.texcoord1.x);
  return Bend(base_pos, pivot, up, disp, g_Mtd[11].x, saturate(world.y - pivot.y));
#else
  const float3 disp = ForceDisplacement(inst, pivot, t + g_Mtd[10].PHASE_COMP * input.texcoord1.x);
  return Bend(world, pivot, up, disp, g_Mtd[10].BEND_COMP, 1 - input.texcoord2.z);
#endif
}

// Current and previous world positions of the vertex, sharing everything that does not depend on time or camera.
void FusedPoses(Input input, float time, float dt, out float3 cur, out float3 prev) {
  const uint inst = input.instance;
  const int base = (int)(asint(g_Instance[IndirectInstance(input) * 13 + 4].x) + (int)input.normal.w) * 3;
  const float4 p = float4(input.position, 1);
  const float3 world = float3(dot(g_ObjMatrix[base], p), dot(g_ObjMatrix[base + 1], p), dot(g_ObjMatrix[base + 2], p));
#ifdef STATIC
  cur = world;
  prev = world;
#else
  const float4 r0 = g_Instance[inst * 13], r1 = g_Instance[inst * 13 + 1], r2 = g_Instance[inst * 13 + 2];
#ifdef LOD1
  const float4 q = float4(input.texcoord2.xyz, 1);
#else
  const float4 q = float4(input.texcoord3.xyz, 1);
#endif
  const float3 pivot = float3(dot(r0, q), dot(r1, q), dot(r2, q));
  const float3 up = normalize(float3(r0.y, r1.y, r2.y));
#ifdef LOD1
  const float4 m = g_Mtd[10];
  const float keep = 1 - m.z, range = 1 / m.x;
  const float fade_cur = saturate((length(g_Scene[kCameraPos].xyz - world) - m.y) * range);
  const float fade_prev = saturate((length(g_Scene[kPrevCameraPos].xyz - world) - m.y) * range);
  const float3 base_cur = world + fade_cur * (pivot - world) * keep;
  const float3 base_prev = world + fade_prev * (pivot - world) * keep;
  const float t = time + m.w * input.texcoord1.x;
  const float bend_scale = g_Mtd[11].x;
  const float weight = saturate(world.y - pivot.y);
#else
  const float3 base_cur = world, base_prev = world;
  const float t = time + g_Mtd[10].PHASE_COMP * input.texcoord1.x;
  const float bend_scale = g_Mtd[10].BEND_COMP;
  const float weight = 1 - input.texcoord2.z;
#endif
  float3 disp_cur, disp_prev;
  ForceDisplacements(inst, pivot, float2(t, t - dt), disp_cur, disp_prev);
  cur = Bend(base_cur, pivot, up, disp_cur, bend_scale, weight);
  prev = Bend(base_prev, pivot, up, disp_prev, bend_scale, weight);
#endif
}

// Pulls the position slightly towards the camera (reverse-Z) so the greater-equal depth test against the game's
// depth keeps this surface despite tiny differences from the game's own shader.
static const float kZScale = 1.0001;

Vertex VSMain(Input input) {
  const float time = g_Frame[27].x, dt = g_Frame[27].z;
  Vertex v;
#ifdef UNFUSED
  v.world = WorldPosition(input, time, g_Scene[kCameraPos].xyz);
  const float3 prev_world = WorldPosition(input, time - dt, g_Scene[kPrevCameraPos].xyz);
#else
  float3 prev_world;
  FusedPoses(input, time, dt, v.world, prev_world);
#endif
  v.cur = Clip(v.world, kCameraPos, kViewProj);
  v.prev = Clip(prev_world, kPrevCameraPos, kPrevViewProj);
  v.position = v.cur;
  v.position.z *= kZScale;
  const float2 size = g_Scene[kScreenSize].xy;
  const float2 encode = g_Scene[kVelocityEncode].xy;
  v.encode = float3(float2(0.5 * size.x / size.y, -0.5) * encode.x, encode.y);
  v.uv = float2(input.texcoord0) * (1.0 / 2048) * g_TexScroll.xy + g_TexScroll.zw;
  v.instance = IndirectInstance(input);
  return v;
}

struct Output {
  float2 velocity : SV_Target0;  // the game's encoding: (cur - prev) NDC * (0.5 * width / height, -0.5)
  float depth : SV_Target1;      // this surface's depth, for the merge
};

// Cost measurement: rasterization and pixel shader launches without any pixel work.
void PSDiscard() { discard; }

Output PSMain(Vertex v) {
#if ALPHA_TEST
  if (AlphaDiscarded(v.uv, v.world, v.position.xy, v.instance)) discard;
#endif
  const float2 delta = v.cur.xy / v.cur.w - v.prev.xy / v.prev.w;
  Output o;
  o.velocity = delta * v.encode.xy + v.encode.z;
  o.depth = v.cur.z / v.cur.w;
  return o;
}
