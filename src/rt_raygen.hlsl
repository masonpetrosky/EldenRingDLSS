// Ray generation shaders added to the game's ray tracing pipeline (rt_pipeline.hpp), compiled as a DXIL library.
// ErdlssShadowRayGen is a port of the game's GXRayGen_dirLightShadow (sun shadows at the render size, from the DXIL
// disassembly of capture er117_rtmax_scale50_f9082); ErdlssShadowAoRayGen also traces the AO at the render size
// (the game's GXRayGen_ao runs at half size) and writes it to the output's 4th channel, which the game leaves at 0.
// ErdlssShadowAoRayGen also replaces the game's random numbers. Its hash, sin(dot(pixel / size + frameRandom, k)),
// forms diagonal bands that the per-frame offset sweeps across the screen; the game's denoisers blur them away, but
// Ray Reconstruction keeps them as a moving "mirage" in distant grass. PCG gives independent numbers per pixel, per
// frame (from frameRandom, fresh every frame) and per stream (shadow and AO rays apart). ErdlssShadowAoBlueRayGen
// draws them from blue-noise tiles instead (blue_noise.hlsli), so each frame's samples are spread evenly over the
// screen: the shader can only use the game's bindings, so the tiles are a constant array rather than a texture.
// ErdlssShadowAoTemporalRayGen animates the blue noise over frames instead of shuffling it: each pixel steps its
// blue-noise value along the R2 sequence, so a pixel's successive samples cover the directions evenly (low discrepancy
// in time, which Ray Reconstruction's accumulation converges on faster) while every frame stays spatially blue.

#include "blue_noise.hlsli"
// The bindings are the game's: the global root signature's g_sceneCB (b0), scene (t64) and output (u0), and the
// ray generation record's local root signature: l_rayGenCB (b16), G-buffer normal (t67) and depth (t68).

cbuffer g_sceneCB : register(b0) {
  float4 g_viewToWorld[3];  // one register per world axis: dot(r.xyz, view) + r.w (column_major float4x3 mtxCamera)
  float2 g_fovTan;
  float2 g_clipDist;
  float3 g_lightDir;
  float g_lightSharpness;
  float4 g_lightDiffColor;
  float4 g_lightSpecColor;
  int g_useMipLevel;
  float g_mipLevelOffset;
  float2 g_bufferSizeRatio;
  int g_rayTypeRadianceIndex;
  int g_rayTypeShadowIndex;
  int g_rayTypeIndexReserve;
  int g_rayTypeNum;
  float4 g_screenPercentage;
  int4 g_debug;
  float4 g_clipToView[4];  // one register per view component (column_major float4x4 mtxInvProj)
  float g_frameRandom;
  uint g_frameIndex;  // the game's padding (padd4); the add-on writes the frame number here for ErdlssShadowAoTemporalRayGen
};

cbuffer l_rayGenCB : register(b16) {
  uint l_aoSampleCount;
  uint l_shadowSampleCount;
  float l_shadowConeRadius;
  uint l_adjustRayStartPosition;
  uint2 l_aoTextureSize;
  uint2 l_shadowTextureSize;
  uint2 l_radTextureSize;
  uint2 l_dummy;
  float2 l_aoRayLengthRange;
  float2 l_radianceRayLengthRange;
  float2 l_shadowRayLengthRange;
  uint l_enableAoAlpha;
  uint l_enableShadowAlpha;
  float2 l_aoScaleOffset;
  float l_shadowMaxDist;
  float l_aoMaxDist;
};

RaytracingAccelerationStructure g_scene : register(t64);
Texture2D<float4> l_normal : register(t67);
Texture2D<float> l_depth : register(t68);
RWTexture2D<float4> g_output : register(u0);

struct ShadowPayload {
  int blocked;  // the game's RayPayload_shadow: set before tracing, cleared by its miss shader
};

// The game's constants, bit for bit.
static const float kHashX = asfloat(0x41D38312);      // 26.439
static const float kHashY = asfloat(0x4240F1AA);      // 48.236
static const float kHashMul1 = asfloat(0x43ADFCCD);   // 347.975
static const float kHashMul2 = asfloat(0x44108EA8);   // 578.229
static const float kOffsetPerMeter = asfloat(0x38D1B717);  // 1e-4
static const float kTwoPi = asfloat(0x40C90FDB);
static const float kInv255 = asfloat(0x3B808081);
static const uint kOpaqueFlags = RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
                                 RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_CULL_BACK_FACING_TRIANGLES;  // 29
static const uint kAlphaFlags = kOpaqueFlags & ~RAY_FLAG_FORCE_OPAQUE;                                      // 28

float Hash(float2 pixel, float2 size) {
  const float2 p = pixel / size + g_frameRandom;
  return frac(frac(sin(dot(p, float2(kHashX, kHashY))) * kHashMul1) * kHashMul2);
}

// PCG hash (Jarzynski and Olano, 2020), as in ao_upscale.hlsl.
uint Pcg(uint v) {
  const uint state = v * 747796405u + 2891336453u;
  const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}

// Two uniform numbers in [0, 1) for this pixel, frame and stream.
float2 PixelRandom(uint2 pixel, uint stream) {
  const uint seed = Pcg(pixel.x ^ Pcg(pixel.y ^ Pcg(asuint(g_frameRandom) + stream)));
  const uint a = Pcg(seed), b = Pcg(a);
  return float2(a >> 8, b >> 8) / 16777216.0f;
}

float BlueNoise(uint2 p, uint tile) {
  const uint i = (tile * kBlueNoiseSize + p.y % kBlueNoiseSize) * kBlueNoiseSize + p.x % kBlueNoiseSize;
  return (float((kBlueNoise[i >> 2] >> ((i & 3) * 8)) & 255) + 0.5f) / 256.0f;
}

// Two blue-noise numbers in [0, 1) for this pixel, frame and stream: the tiles shifted by a random offset per frame
// and stream (each frame's pattern stays blue) and rotated by a random amount (Cranley-Patterson).
float2 PixelBlueNoise(uint2 pixel, uint stream) {
  const uint seed = Pcg(asuint(g_frameRandom) + stream);
  const uint2 shift = uint2(seed & 0xFFFF, seed >> 16) % kBlueNoiseSize;
  const float2 rotation = float2(Pcg(seed) >> 8, Pcg(seed ^ 0x9E3779B9u) >> 8) / 16777216.0f;
  return frac(float2(BlueNoise(pixel + shift, 0), BlueNoise(pixel + shift, 1)) + rotation);
}

// Blue noise animated by frame: the pixel's blue-noise value plus frame * (R2's constants 0.7548776662, 0.5698402910),
// modulo 1, in 32-bit fixed point so it stays exact for any frame number. Streams use the tiles at fixed offsets.
float2 PixelTemporalBlueNoise(uint2 pixel, uint stream) {
  const uint2 p = pixel + stream * uint2(32, 21);
  const float2 base = float2(BlueNoise(p, stream & 1), BlueNoise(p, (stream + 1) & 1));
  const uint2 step = g_frameIndex * uint2(3242174890u, 2447445414u);
  return frac(base + float2(step >> 8) / 16777216.0f);
}

float RadicalInverse(uint i) {
  float result = 0.0f, bit = 0.5f;
  for (; i != 0; i >>= 1) {
    if (i & 1) result += bit;
    bit *= 0.5f;
  }
  return result;
}

void Basis(float3 n, out float3 t, out float3 b) {
  const float3 up = abs(n.y) < asfloat(0x3F666666) ? float3(0, 1, 0) : float3(0, 0, 1);  // 0.9
  t = normalize(cross(up, n));
  b = cross(t, n);
}

bool Blocked(uint flags, uint mask, float3 origin, float tmin, float3 direction, float tmax) {
  RayDesc ray;
  ray.Origin = origin;
  ray.TMin = tmin;
  ray.Direction = direction;
  ray.TMax = tmax;
  ShadowPayload payload;
  payload.blocked = 1;
  TraceRay(g_scene, flags, mask, g_rayTypeShadowIndex, g_rayTypeNum, g_rayTypeShadowIndex, ray, payload);
  return payload.blocked != 0;
}

struct Surface {
  uint2 pixel;
  float2 size;
  float3 world;
  float viewZ;
  float3 normal;  // decoded but not normalized, as the game's shadow shader uses it
  float random1, random2;  // the game's hash
};

Surface LoadSurface() {
  Surface s;
  s.pixel = DispatchRaysIndex().xy;
  s.size = float2(DispatchRaysDimensions().xy);
  const float depth = l_depth.Load(int3(s.pixel, 0));
  const float2 uv = (float2(s.pixel) + 0.5f) / s.size;
  const float4 clip = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f);
  const float3 view = float3(dot(g_clipToView[0], clip), dot(g_clipToView[1], clip), dot(g_clipToView[2], clip)) /
                      dot(g_clipToView[3], clip);
  s.world = float3(dot(g_viewToWorld[0].xyz, view) + g_viewToWorld[0].w,
                   dot(g_viewToWorld[1].xyz, view) + g_viewToWorld[1].w,
                   dot(g_viewToWorld[2].xyz, view) + g_viewToWorld[2].w);
  s.viewZ = view.z;
  s.normal = l_normal.Load(int3(s.pixel, 0)).xyz * 2.0f - 1.0f;
  s.random1 = Hash(float2(s.pixel), s.size);
  s.random2 = Hash(float2(s.pixel) + 128.0f, s.size);
  return s;
}

// GXRayGen_dirLightShadow: rays toward the sun inside a cone, the fraction that reaches the sky.
float SunShadow(Surface s, float2 random) {
  const float offset = s.viewZ * kOffsetPerMeter;
  const float tmin = l_shadowRayLengthRange.x + offset;
  const float tmax = l_shadowRayLengthRange.y + offset;
  float3 origin = s.world;
  float rayMin = tmin;
  if (l_adjustRayStartPosition != 0) {
    origin = s.world + s.normal * tmin;
    rayMin = 0.0f;
  }
  const bool far = s.viewZ >= l_shadowMaxDist;
  const uint count = far ? 1 : l_shadowSampleCount;
  const float cone = far ? 0.0f : l_shadowConeRadius;
  const uint flags = l_enableShadowAlpha != 0 && !far ? kAlphaFlags : kOpaqueFlags;
  float3 t, b;
  Basis(g_lightDir, t, b);
  float lit = 0.0f;
  for (uint i = 0; i < count; ++i) {
    const float u = (float(i) + random.x) / float(count);
    float v = RadicalInverse(i) + random.y;
    if (v > 1.0f) v -= 1.0f;
    const float r = sqrt(u) * cone;
    const float phi = v * kTwoPi;
    const float3 direction = g_lightDir + b * (r * cos(phi)) + t * (r * sin(phi));
    if (!Blocked(flags, 2, origin, rayMin, direction, tmax)) lit += 1.0f;
  }
  return lit / float(count);
}

// GXRayGen_ao's sampling (cosine-weighted hemisphere around the normal) at the render size; the fraction of rays
// that leave unoccluded, through the game's scale and offset.
float AmbientOcclusion(Surface s, float2 random) {
  float open = 1.0f;
  if (s.viewZ < l_aoMaxDist && l_aoSampleCount != 0) {
    const float3 n = normalize(s.normal);
    const uint flags = l_enableAoAlpha != 0 ? kAlphaFlags : kOpaqueFlags;
    float3 t, b;
    Basis(n, t, b);
    float unoccluded = 0.0f;
    for (uint i = 0; i < l_aoSampleCount; ++i) {
      const float u = (float(i) + random.x) / float(l_aoSampleCount);
      float v = RadicalInverse(i) + random.y;
      if (v > 1.0f) v -= 1.0f;
      const float r = sqrt(u);
      const float phi = v * kTwoPi;
      const float3 direction = n * sqrt(1.0f - u) + b * (r * cos(phi)) + t * (r * sin(phi));
      if (!Blocked(flags, 1, s.world, l_aoRayLengthRange.x, direction, l_aoRayLengthRange.y)) unoccluded += 1.0f;
    }
    open = unoccluded / float(l_aoSampleCount);
  }
  return saturate(l_aoScaleOffset.x * open + l_aoScaleOffset.y);
}

// The game's output layout: x = shadow, y/z = the view depth as a half float split into two bytes (for the
// denoiser's depth weights), w = unused (0) or our AO.
void Store(Surface s, float shadow, float ao) {
  const uint depth16 = f32tof16(s.viewZ);
  g_output[s.pixel] = float4(shadow, float(depth16 >> 8) * kInv255, float(depth16 & 255) * kInv255, ao);
}

[shader("raygeneration")]
void ErdlssShadowRayGen() {
  const Surface s = LoadSurface();
  Store(s, SunShadow(s, float2(s.random1, s.random2)), 0.0f);
}

[shader("raygeneration")]
void ErdlssShadowAoRayGen() {
  const Surface s = LoadSurface();
  Store(s, SunShadow(s, PixelRandom(s.pixel, 0)), AmbientOcclusion(s, PixelRandom(s.pixel, 1)));
}

[shader("raygeneration")]
void ErdlssShadowAoBlueRayGen() {
  const Surface s = LoadSurface();
  Store(s, SunShadow(s, PixelBlueNoise(s.pixel, 0)), AmbientOcclusion(s, PixelBlueNoise(s.pixel, 1)));
}

[shader("raygeneration")]
void ErdlssShadowAoTemporalRayGen() {
  const Surface s = LoadSurface();
  Store(s, SunShadow(s, PixelTemporalBlueNoise(s.pixel, 0)), AmbientOcclusion(s, PixelTemporalBlueNoise(s.pixel, 1)));
}
