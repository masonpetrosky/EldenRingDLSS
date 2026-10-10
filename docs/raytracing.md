# Ray tracing: making it affordable, and DLSS Ray Reconstruction

The user's goal: make Elden Ring's ray tracing a practical option instead of an extremely expensive one. Upscaling
(stage 3) is the base for that. This note collects what is known and the plan (2026-10-10).

## Elden Ring's ray tracing

Patch 1.09 (March 2023) added ray-traced **shadows and ambient occlusion**, no reflections. FromSoftware's notes warn
of large frame-rate drops and recommend 1080p; reviews found a small visual gain (mostly vegetation shadows outdoors,
AO on rocks and cliffs) for a severe cost, with no upscaler to offset it. Settings: off, low, medium, high, maximum.

Not yet examined in this project: which passes trace rays (DXR pipelines or inline ray queries in compute), at which
resolution they run (do they follow the dynamic resolution, i.e. get cheaper with the render scale?), how much of the
cost is acceleration-structure building, and how the game denoises the results. All captures so far have ray
tracing off.

## Preset M cost (solved)

DLSS presets L and M cost what the size the feature is created for costs (DLSS programming guide 310.6, section
3.2.2.1). The add-on created the feature for the mode's dynamic maximum (the full output), so M at 50% ran like a
4K input. `tools/dlss_bench` (DLSS timed outside the game), RTX 5070 Ti, 4K output:

| Mode (input) | Preset | Created for | GPU time |
| --- | --- | --- | --- |
| DLAA (2160p) | K / M | 2160p | 1.66 / 6.01 ms |
| Quality (1448p) | K | either | 1.55 ms |
| Quality (1448p) | M | 2160p / 1448p | 5.90 / 3.28 ms |
| Performance (1080p) | K | either | 1.52 ms |
| Performance (1080p) | M | 2160p / 1080p | 5.87 / 2.15 ms |
| Performance (1080p) | L | 1080p | 2.72 ms |

Fixed in commit 3c76c9d: the feature is created for the engine's render size. NVIDIA's table for the same case
(Performance, 4K): RTX 5070 K 2.11 / M 2.98 ms, RTX 5080 K 1.31 / M 1.74 ms. M is meant for upscaling; at DLAA it is
3.6x K. (Luma's Persona 5 Royal mod creates DLSS for the real render size, so its M cost there likely came from
running at native resolution, where M is expensive.)

## DLSS Ray Reconstruction (RR)

From NVIDIA's DLSS-RR Integration Guide (github.com/NVIDIA/DLSS, doc/):
- One model that denoises any ray-traced signal and upscales; it replaces DLSS-SR and the game's denoisers. Same
  placement as DLSS-SR (before tonemapping, depth of field after it). Same quality modes. No dynamic resolution
  (fixed render size, which the add-on now uses anyway). No exposure input. At least 32 jitter positions recommended.
- Separate library `nvngx_dlssd.dll` (fetched by `tools/fetch_dlss_sdk.ps1` into `third_party/dlss/bin/rel|dev`). Headers are already in `third_party/dlss/include` (`nvsdk_ngx_defs_dlssd.h`, helpers).
- Cost on the RTX 5070 Ti, 4K output (`tools/dlss_bench ... rr`, feature created for the render size): Performance
  (1080p in) 4.0 ms with the default preset (F), 3.5 ms with D or E; Quality (1448p) 6.3 ms; DLAA 12.4 ms. DLSS-SR at
  Performance: K 1.5 ms, M 2.15 ms. So RR costs about 2 ms more than SR at 50% and about 4.8 ms more at 67%; it pays
  off only if it replaces more than that in the game's denoising, or lets the game trace fewer rays.
- Inputs at render resolution: the noisy lit color, diffuse albedo, specular albedo (to be derived: average specular
  reflectance for the view direction, from F0/metalness and roughness), shading normals, linear roughness, depth,
  motion vectors; for reflections, specular hit distance or reflection motion vectors (Elden Ring has no RT
  reflections, so not needed). Sky defaults: diffuse albedo 0.5, specular albedo 0, normal 0, roughness 0.
- The noisy input must come from independent samples: no screen-space dithering, no checkerboarding, no sampling
  patterns shared across the screen, no correlated hashes. Games that trace for their own denoiser often use blue
  noise or interleaved patterns; that is the main risk for Elden Ring.

### What porting RR to Elden Ring would take

1. Find the ray tracing passes and the game's shadow and AO denoisers (a RenderDoc capture with ray tracing on).
2. Skip the denoisers so lighting uses the raw ray-traced shadows and AO; the lit image before the TAA becomes the
   noisy input.
3. Build the guide buffers from the G-buffer (albedo, normals, roughness; specular albedo computed) at render size.
4. Evaluate DLSS-RR instead of DLSS-SR at the TAA, everything after it as now (full-size post-processing).
5. If the game's sampling is correlated, replace its random-number generation in the ray tracing shaders (the add-on
   already replaces game shaders for the wind prepass) or add per-frame decorrelation.

## Plan, in order

1. **Measure ray tracing as it is**, with the upscaling build: PresentMon at RT off / low / max, at DLAA, 67% and 50%,
   plus a RenderDoc capture (RT on, 67%) to list the ray tracing and denoise passes, their resolutions and their GPU
   times (RenderDoc can time passes). This answers whether the rays already get cheaper with the render scale and
   where the remaining cost is (acceleration structures, tracing, denoising).
2. If ray tracing passes do not follow the render scale, make them (same tools as the post-processing corrections).
3. Then decide on RR with numbers: its cost on this GPU (above) against the game's denoising cost, and whether the
   game's sampling suits it.

## Adding ray-traced reflections or GI (feasibility, 2026-10-10)

The user asked whether the add-on could add ray-traced reflections or global illumination.
- ReShade's add-on API reports acceleration-structure builds (`build_acceleration_structure`, with geometry and
  instance inputs) and `dispatch_rays`, so with the game's ray tracing on, the add-on can see the scene structure the
  game builds for its shadow and AO rays and trace its own rays against it (own DXR or inline-ray-query shaders on
  the game's device).
- Denoising: DLSS-RR is designed for exactly these signals.
- Insertion: reflections would replace or augment the game's screen-space reflections (`GXSsr*` passes); GI would
  add indirect light in the lighting pass.
- The hard part is shading at hit points (material and lighting): each hit triangle's textures and UVs, plus sun,
  local lights and sky. If the game's shadow rays already look up materials (alpha-tested foliage), a per-instance
  material table exists to reuse. Otherwise start with the hybrid: on-screen hits take the lit screen color,
  off-screen hits fall back to sky/ambient.
- Order: reflections first (about one ray per pixel on glossy surfaces, replaces SSR, changes the look least), GI
  second (many diffuse rays, full lighting at hits, bigger change to the art direction).
- Deciding questions for the RT-on capture: DXR pipelines with hit shaders or inline ray queries; what the
  acceleration structure contains (terrain, buildings, foliage, characters); whether instances carry material or
  texture indices.
