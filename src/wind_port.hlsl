// Motion vectors for the game's force-driven vegetation (M[AMSN_V]_Grass2, _Vesitation, _Grass2_LOD1): a port of the
// position math of their G-buffer vertex shaders (decompiled from M[AMSN_V]_Grass2_Gbuf and _Grass2_LOD1_Gbuf),
// evaluated for the current and the previous frame in one pass. Drawn with the game's root signature right after the
// game's own draw, into a velocity target (the game's motion encoding) and a depth target that the TAA-time merge
// compares with the final depth.
//
// Per material, PHASE_COMP and BEND_COMP pick the cbMtdParam components (byte 160) that scale the per-vertex wind
// phase (TEXCOORD1.x) and the bend angle: Grass2 uses .y / .z, Vesitation .x / .w. LOD1 builds the LOD1 variant:
// pivot in TEXCOORD2, blades shrinking towards the pivot with camera distance (byte 160: .x range, .y start,
// .z keep, .w phase scale), bend scale at byte 176.x, bend weighted by the height above the pivot.
//
// ALPHA_TEST ports the alpha test of the materials' depth-prepass pixel shaders (M[AMSN_V]_Grass2_GBufDpt and
// _Vesitation_GBufDpt): texture alpha, camera-distance fades with an 8x8 dither, per-instance LOD dither and
// FC_AlphaRef. 1: Grass2 (one far fade, cbMtdParam byte 144), 2: Vesitation (plus a near fade, byte 160). Without it,
// transparent parts of blade cards would record their depth over the blades behind them.
// SPDX-License-Identifier: MIT

#ifndef PHASE_COMP
#define PHASE_COMP y
#define BEND_COMP z
#endif

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
  float4 g_MatDyn;  // .w: FC_ColorSlotNo
};
cbuffer cbModelParam : register(b7) {
  float4 g_Model[3];  // [2].x: FC_DitherEnable, .y: FC_DitherFadePatternSwitch
};
Texture2D<float4> g_Material : register(t0);
Texture2D<float4> g_DitherTexture : register(t38);
SamplerState SS_DiffuseMap : register(s0);
SamplerState SS_WrapPoint : register(s6);

struct ForceParam {
  int4 flags;      // x: directional, y: gusts, z: cone
  float4 origin;   // xyz position, w: constant push
  float4 dir;      // xyz direction, w: amplitude
  float4 wave;     // x: period, y: radius, w: side sway
  float4 gust;     // x: gust length, y: gust offset, z: gust strength
  float4 pulse;    // x: speed, y: rise, z: fall, w: pause
};
ByteAddressBuffer g_InstanceIndexBuffer : register(t25);
StructuredBuffer<ForceParam> g_forceParam : register(t26);

// cbSceneParam float4 indices.
static const uint kScreenSize = 1, kCameraPos = 3, kViewProj = 24, kPrevCameraPos = 28, kPrevViewProj = 29,
                  kCameraFadeEnable = 69, kVelocityEncode = 70, kRenderCameraPos = 71;

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

float FracSigned(float x) { return x >= -x ? frac(abs(x)) : -frac(abs(x)); }

// Summed displacement of all forces acting on this instance at wind time `t`.
float3 ForceDisplacement(uint instance, float3 pivot, float t) {
  const int4 row7 = asint(g_Instance[instance * 13 + 7]);
  const uint count = min((uint)row7.y, 8u);
  float3 acc = 0;
  uint packed = (uint)row7.z;
  for (uint i = 0; i < count; ++i) {
    const ForceParam f = g_forceParam[packed & 255];
    const bool directional = f.flags.x == 1, gusts = f.flags.y == 1, cone = f.flags.z == 1;
    const float3 d = pivot - f.origin.xyz;
    const float3 v = directional ? f.dir.xyz : d;
    if (dot(v, v) > 0) {
      const float dist = sqrt(dot(d, d));
      const float3 vn = v * rsqrt(dot(v, v));
      float falloff = saturate(1 - dist / max(f.wave.y, 0.001));
      if (cone) falloff *= saturate(dot(vn, d * rsqrt(dot(d, d))) * 0.8 + 0.2);
      if (falloff > 0) {
        const bool plane = directional && !cone;
        const float omega = 6.28318 / max(f.wave.x, 0.001);
        const float along = dot(vn.xz, d.xz);
        const float s = sin(omega * t + (plane ? 0 : dist * -4) - omega * (plane ? 1 : 0) * along);
        const float sway = s * f.dir.w;
        const float side = directional ? f.wave.w * f.dir.w * s : 0;
        float pulse = 0;
        if (gusts) {
          const float len = f.gust.x * 2;
          const float cycle = f.pulse.y + f.pulse.z + f.pulse.w;
          const float across = dot(float2(vn.z, -vn.x), d.xz) / len;
          const float wobble = sin(f.gust.x * 12.5664 * FracSigned(across) / len) * min(f.gust.x, 5);
          const float run = f.pulse.x * t;
          const float phase = (run - f.gust.x * 0.25 - along - wobble) / max(cycle * f.pulse.x, 0.001);
          const float fr = phase - floor(phase);
          const float p = phase < 0 ? 1 - fr : fr;
          const float rise = f.pulse.y / cycle;
          float shape = 0;
          if (p < rise) shape = p / rise;
          else if (p < (f.pulse.y + f.pulse.z) / cycle) shape = 1 - (p - rise) * cycle / f.pulse.z;
          pulse = shape * f.gust.z * abs(sin(6.28318 / max(f.gust.x, 0.001) * (run - along)) + f.gust.y);
        }
        const float push = sway + f.origin.w + pulse;
        acc += float3(falloff * 0.05 * (push * vn.x - side * vn.z), vn.y * 0.05 * falloff * push,
                      falloff * 0.05 * (push * vn.z + side * vn.x));
      }
    }
    packed = i != 3 ? packed >> 8 : (uint)row7.w;
  }
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
#ifdef LOD1
  const float4 m = g_Mtd[10];
  const float fade = saturate((length(camera - world) - m.y) * (1 / m.x));
  const float3 base_pos = world + fade * (pivot - world) * (1 - m.z);
  const float3 disp = ForceDisplacement(inst, pivot, t + m.w * input.texcoord1.x);
  const float bend_scale = g_Mtd[11].x;
  const float weight = saturate(world.y - pivot.y);
#else
  const float3 base_pos = world;
  const float3 disp = ForceDisplacement(inst, pivot, t + g_Mtd[10].PHASE_COMP * input.texcoord1.x);
  const float bend_scale = g_Mtd[10].BEND_COMP;
  const float weight = 1 - input.texcoord2.z;
#endif
  const float len = sqrt(dot(disp, disp));
  if (len < 0.00011) return base_pos;
  // Bend around the pivot: rotate by log2(|disp| + 1) * k about the axis perpendicular to the instance's up and the
  // displacement.
  const float3 up = normalize(float3(r0.y, r1.y, r2.y));
  const float3 axis = normalize(cross(up, disp * rsqrt(dot(disp, disp))));
  const float angle = log2(len + 1) * bend_scale;
  const float3 center = dot(axis, base_pos - pivot) * axis + pivot;
  const float3 radial = base_pos - center;
  const float3 bent = center + cos(angle) * radial + sin(angle) * cross(axis, radial);
  return base_pos + (bent - base_pos) * weight;
}

float4 Clip(float3 world, uint camera, uint vp) {
  const float4 rel = float4(world - g_Scene[camera].xyz, 1);
  return float4(dot(g_Scene[vp], rel), dot(g_Scene[vp + 1], rel), dot(g_Scene[vp + 2], rel), dot(g_Scene[vp + 3], rel));
}

// Pulls the position slightly towards the camera (reverse-Z) so the greater-equal depth test against the game's
// depth keeps this surface despite tiny differences from the game's own shader.
static const float kZScale = 1.0001;

Vertex VSMain(Input input) {
  const float time = g_Frame[27].x, dt = g_Frame[27].z;
  Vertex v;
  v.world = WorldPosition(input, time, g_Scene[kCameraPos].xyz);
  v.cur = Clip(v.world, kCameraPos, kViewProj);
  v.prev = Clip(WorldPosition(input, time - dt, g_Scene[kPrevCameraPos].xyz), kPrevCameraPos, kPrevViewProj);
  v.position = v.cur;
  v.position.z *= kZScale;
  const float2 size = g_Scene[kScreenSize].xy;
  const float2 encode = g_Scene[kVelocityEncode].xy;
  v.encode = float3(float2(0.5 * size.x / size.y, -0.5) * encode.x, encode.y);
  v.uv = float2(input.texcoord0) * (1.0 / 2048) * g_TexScroll.xy + g_TexScroll.zw;
  v.instance = IndirectInstance(input);
  return v;
}

#if ALPHA_TEST
// The game's depth-prepass alpha test: true where the game discarded this fragment.
bool Discarded(Vertex v) {
  const float alpha = g_Material.Sample(SS_DiffuseMap, v.uv * g_Tiling.xy + g_Tiling.zw).a;
  const float dist = length(g_Scene[kRenderCameraPos].xyz - v.world);
  const bool fading = asint(g_Scene[kCameraFadeEnable].w) != 0;
  const float dither = g_DitherTexture.Sample(SS_WrapPoint, v.position.xy * 0.125).a;
  const float threshold = dither * 0.99 + 0.01;
  const float4 far_fade = g_Mtd[9];  // byte 144: .x end, .y start
  const float far = fading ? (dist - far_fade.y) / max(far_fade.x - far_fade.y, 0.001) : 1;
  float visible = far >= threshold ? 1 : 0;
#if ALPHA_TEST == 2
  const float4 near_fade = g_Mtd[10];  // byte 160: .y start, .z end
  const float near = fading ? (dist - near_fade.y) / max(near_fade.z - near_fade.y, 0.001) : 1;
  visible = fading ? visible * (1 - (near >= threshold ? 1 : 0)) : 1;
#endif
  const float noise = dither - 0.5;
  const float4 alpha_ref = g_Mtd[1];  // FC_AlphaRef
  const float4 dither_params = g_Model[2];
  if (dither_params.x > 0 && dither_params.y * noise * 0.999 + g_Instance[v.instance * 13 + 7].x < 0.5) return true;
  const float lod = g_Instance[v.instance * 13 + 9 + (int)g_MatDyn.w].w;
  if (lod < 1 && lod + noise * 0.999 < 0.5) return true;
  return noise * alpha_ref.y + visible * alpha < alpha_ref.x;
}
#endif

struct Output {
  float2 velocity : SV_Target0;  // the game's encoding: (cur - prev) NDC * (0.5 * width / height, -0.5)
  float depth : SV_Target1;      // this surface's depth, for the merge
};

Output PSMain(Vertex v) {
#if ALPHA_TEST
  if (Discarded(v)) discard;
#endif
  const float2 delta = v.cur.xy / v.cur.w - v.prev.xy / v.prev.w;
  Output o;
  o.velocity = delta * v.encode.xy + v.encode.z;
  o.depth = v.cur.z / v.cur.w;
  return o;
}
