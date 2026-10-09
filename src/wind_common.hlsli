// Wind math shared by wind_port.hlsl (the ported vertex shader) and wind_prepass.hlsl (the pixel shader that
// replaces the game's grass depth prepass): the force displacement, the blade bend and its inverse, and the
// materials' alpha tests. The including file declares g_Mtd (cbMtdParam), g_Instance (cbInstanceData) and g_Scene
// (cbSceneParam), for ALPHA_TEST also g_Tiling, g_MatDyn, g_Model, g_Material, g_DitherTexture, SS_DiffuseMap and
// SS_WrapPoint, and defines FORCE_PARAM_REGISTER, the register of g_forceParam.
//
// Per material, PHASE_COMP and BEND_COMP pick the cbMtdParam components (byte 160) that scale the per-vertex wind
// phase (TEXCOORD1.x) and the bend angle: Grass2 uses .y / .z, Vesitation .x / .w.
// SPDX-License-Identifier: MIT

#ifndef PHASE_COMP
#define PHASE_COMP y
#define BEND_COMP z
#endif

struct ForceParam {
  int4 flags;      // x: directional, y: gusts, z: cone
  float4 origin;   // xyz position, w: constant push
  float4 dir;      // xyz direction, w: amplitude
  float4 wave;     // x: period, y: radius, w: side sway
  float4 gust;     // x: gust length, y: gust offset, z: gust strength
  float4 pulse;    // x: speed, y: rise, z: fall, w: pause
};
StructuredBuffer<ForceParam> g_forceParam : register(FORCE_PARAM_REGISTER);

// cbSceneParam float4 indices.
static const uint kScreenSize = 1, kCameraPos = 3, kInvViewProj = 8, kViewProj = 24, kPrevCameraPos = 28,
                  kPrevViewProj = 29, kCameraFadeEnable = 69, kVelocityEncode = 70, kRenderCameraPos = 71;

float FracSigned(float x) { return x >= -x ? frac(abs(x)) : -frac(abs(x)); }

// Summed displacement of all forces acting on an instance (at its pivot) for two wind times at once
// (x: current, y: previous); everything but the time terms is shared.
void ForceDisplacements(uint instance, float3 pivot, float2 t, out float3 acc_cur, out float3 acc_prev) {
  const int4 row7 = asint(g_Instance[instance * 13 + 7]);
  const uint count = min((uint)row7.y, 8u);
  acc_cur = 0;
  acc_prev = 0;
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
        const float2 s = sin(omega * t + ((plane ? 0 : dist * -4) - omega * (plane ? 1 : 0) * along));
        const float2 sway = s * f.dir.w;
        const float2 side = directional ? f.wave.w * f.dir.w * s : 0;
        float2 pulse = 0;
        if (gusts) {
          const float len = f.gust.x * 2;
          const float cycle = f.pulse.y + f.pulse.z + f.pulse.w;
          const float across = dot(float2(vn.z, -vn.x), d.xz) / len;
          const float wobble = sin(f.gust.x * 12.5664 * FracSigned(across) / len) * min(f.gust.x, 5);
          const float2 run = f.pulse.x * t;
          const float2 phase = (run - f.gust.x * 0.25 - along - wobble) / max(cycle * f.pulse.x, 0.001);
          const float2 fr = phase - floor(phase);
          const float2 p = phase < 0 ? 1 - fr : fr;
          const float rise = f.pulse.y / cycle;
          const float2 shape = p < rise ? p / rise
                               : p < (f.pulse.y + f.pulse.z) / cycle ? 1 - (p - rise) * cycle / f.pulse.z
                                                                     : 0;
          pulse = shape * f.gust.z * abs(sin(6.28318 / max(f.gust.x, 0.001) * (run - along)) + f.gust.y);
        }
        const float2 push = sway + f.origin.w + pulse;
        const float k = falloff * 0.05;
        acc_cur += float3(k * (push.x * vn.x - side.x * vn.z), vn.y * k * push.x, k * (push.x * vn.z + side.x * vn.x));
        acc_prev += float3(k * (push.y * vn.x - side.y * vn.z), vn.y * k * push.y, k * (push.y * vn.z + side.y * vn.x));
      }
    }
    packed = i != 3 ? packed >> 8 : (uint)row7.w;
  }
}

// Bend around the pivot: rotate by log2(|disp| + 1) * k about the axis perpendicular to the instance's up and the
// displacement, blended per vertex by `weight`.
float3 Bend(float3 base_pos, float3 pivot, float3 up, float3 disp, float bend_scale, float weight) {
  const float len = sqrt(dot(disp, disp));
  if (len < 0.00011) return base_pos;
  const float3 axis = normalize(cross(up, disp * rsqrt(dot(disp, disp))));
  const float angle = log2(len + 1) * bend_scale;
  const float3 center = dot(axis, base_pos - pivot) * axis + pivot;
  const float3 radial = base_pos - center;
  const float3 bent = center + cos(angle) * radial + sin(angle) * cross(axis, radial);
  return base_pos + (bent - base_pos) * weight;
}

// The inverse of Bend: the unbent position of a bent one. The bend keeps the component along the axis and, in the
// plane across it, maps r to a*r + b*(axis x r) with a = 1 - w + w*cos(angle), b = w*sin(angle), a scaled rotation.
float3 Unbend(float3 bent_pos, float3 pivot, float3 up, float3 disp, float bend_scale, float weight) {
  const float len = sqrt(dot(disp, disp));
  if (len < 0.00011) return bent_pos;
  const float3 axis = normalize(cross(up, disp * rsqrt(dot(disp, disp))));
  const float angle = log2(len + 1) * bend_scale;
  const float3 center = dot(axis, bent_pos - pivot) * axis + pivot;
  const float3 radial = bent_pos - center;
  const float a = 1 - weight + weight * cos(angle), b = weight * sin(angle);
  return center + (a * radial - b * cross(axis, radial)) / (a * a + b * b);
}

float4 Clip(float3 world, uint camera, uint vp) {
  const float4 rel = float4(world - g_Scene[camera].xyz, 1);
  return float4(dot(g_Scene[vp], rel), dot(g_Scene[vp + 1], rel), dot(g_Scene[vp + 2], rel), dot(g_Scene[vp + 3], rel));
}

// The game's motion encoding: (current - previous) NDC * (0.5 * width / height, -0.5) * scale + bias.
float2 EncodeMotion(float4 cur, float4 prev) {
  const float2 size = g_Scene[kScreenSize].xy;
  const float2 encode = g_Scene[kVelocityEncode].xy;
  return (cur.xy / cur.w - prev.xy / prev.w) * float2(0.5 * size.x / size.y, -0.5) * encode.x + encode.y;
}

#if ALPHA_TEST
// The game's alpha test for this material: true where the game discards the fragment. 1: Grass2 (far fade,
// cbMtdParam byte 144), 2: Vesitation (plus a near fade, byte 160), 3: Grass2_LOD1 (FC_CameraFadeParam).
bool AlphaDiscarded(float2 uv, float3 world, float2 pixel, uint instance) {
  const float alpha = g_Material.Sample(SS_DiffuseMap, uv * g_Tiling.xy + g_Tiling.zw).a;
  const float dist = length(g_Scene[kRenderCameraPos].xyz - world);
  const bool fading = asint(g_Scene[kCameraFadeEnable].w) != 0;
  const float dither = g_DitherTexture.Sample(SS_WrapPoint, pixel * 0.125).a;
  const float threshold = dither * 0.99 + 0.01;
#if ALPHA_TEST == 3
  if (asint(g_MatDyn[6].x) != 0) {
    const float4 gradient = g_MatDyn[5];  // far gradient, far intercept, near gradient, near intercept
    const float far = saturate(gradient.x * dist + gradient.y), near = saturate(gradient.z * dist + gradient.w);
    if (g_MatDyn[6].y * (near - far) + far < threshold) return true;
  }
  const float visible = 1;
#else
  const float4 far_fade = g_Mtd[9];  // byte 144: .x end, .y start
  const float far = fading ? (dist - far_fade.y) / max(far_fade.x - far_fade.y, 0.001) : 1;
  float visible = far >= threshold ? 1 : 0;
#endif
#if ALPHA_TEST == 2
  const float4 near_fade = g_Mtd[10];  // byte 160: .y start, .z end
  const float near = fading ? (dist - near_fade.y) / max(near_fade.z - near_fade.y, 0.001) : 1;
  visible = fading ? visible * (1 - (near >= threshold ? 1 : 0)) : 1;
#endif
  const float noise = dither - 0.5;
  const float4 alpha_ref = g_Mtd[1];  // FC_AlphaRef
  const float4 dither_params = g_Model[2];
  if (dither_params.x > 0 && dither_params.y * noise * 0.999 + g_Instance[instance * 13 + 7].x < 0.5) return true;
  const float lod = g_Instance[instance * 13 + 9 + (int)g_MatDyn[0].w].w;
  if (lod < 1 && lod + noise * 0.999 < 0.5) return true;
  return noise * alpha_ref.y + visible * alpha < alpha_ref.x;
}
#endif
