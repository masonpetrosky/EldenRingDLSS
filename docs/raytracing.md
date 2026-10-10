# Ray tracing: making it affordable, and DLSS Ray Reconstruction

The user's goal: make Elden Ring's ray tracing a practical option instead of an extremely expensive one. Upscaling
(stage 3) is the base for that. This note collects what is known and the plan (2026-10-10).

## Elden Ring's ray tracing

Patch 1.09 (March 2023) added ray-traced **shadows and ambient occlusion**, no reflections. FromSoftware's notes warn
of large frame-rate drops and recommend 1080p; reviews found a small visual gain (mostly vegetation shadows outdoors,
AO on rocks and cliffs) for a severe cost, with no upscaler to offset it. Settings: off, low, medium, high, maximum.

### How it works (RenderDoc, 2026-10-10)

Captures `er117_rtmax_scale100_f7202.rdc` and `er117_rtmax_scale50_f9082.rdc` (ray tracing Maximum, night, open
field with grass and trees; the second with the engine scale set to 0.5 through `tools/mem.py`, no add-on).
`tools/rdc_rt.py` lists every action with shader names and the ray tracing calls' parameters. Per frame:

1. About 400 compute dispatches re-animate the moving geometry for the ray tracing scene: SpeedTree trees
   (`SpeedTree_*_Animation`, about 300), grass and bushes (`M[AMSN_V]_Vesitation_Animation`, `..._Edge_`, `[Ov_N]`)
   and skinned characters (`C[...]_Animation_Skin`, `P[ChrCustomize]...`). The rasterizer animates in its vertex
   shaders as before; these copies exist for the acceleration structures.
2. 197 bottom-level acceleration structure refits (`PERFORM_UPDATE | ALLOW_UPDATE | PREFER_FAST_BUILD`), at least
   635k triangles, each after the animation dispatches of its object (groups of 1-3 dispatches, then the refit,
   in one command list).
3. One top-level build (`PREFER_FAST_TRACE`, about 300-320 instances: the ray tracing scene is the area around the
   player, not the whole view distance).
4. Two `DispatchRays` (DXR pipelines with hit groups, 650 hit records): 1920x1080 and 3840x2160 at native,
   960x540 and 1920x1080 at 50%. They follow the render scale. The half-size one is most likely the AO, the
   full-size one the shadows.
5. Denoise in compute: `GXRtPost_VarianceGuidedShadow` at full and half size (temporal: `g_denoiseParam`
   feedbackWeight 0.9 at byte 24, a neighborhood clamp `enableConstrain`, reprojection by `mtxCurToPrevScreen`,
   `prevResolutionScale` for dynamic resolution; inputs only the noisy result and the history), then two
   `GXRtPost_DenoiseATrousFilter` at half size.
6. The raster shadows still run (cascaded shadow maps, screen-space PCF, SSAO); the ray traced results are
   composited with them (`GXRend_CompositeSeparatedShadowMaskBlendPass1/2`).

RenderDoc's GPU duration counters return nothing in headless replay (also for older captures), so the per-pass
cost is still unmeasured; the frame times below and the switches in the dev build (`rtskip`) measure it instead.

### Frame times with the add-on (2026-10-10, 4K, RTX 5070 Ti, GPU busy per frame)

| Setting | RT off | RT Low | RT Max |
| --- | --- | --- | --- |
| 50%, preset K | 8.52 | 14.74 | 13.14 |
| 50%, preset M | 9.15 | 15.34 | 13.81 |
| 67%, preset K | 9.31 | 15.77 | 14.51 |
| DLAA, preset K | 10.98 | 18.15 | 17.38 |
| Game TAA, native | 9.52 | 17.16 | 15.89 |

Each ray tracing setting needs a return to the title screen, so the three columns are from separate loads of the
same spot (Max came out cheaper than Low: the scene changed, probably time of day). Within a column: ray tracing adds
6-7 ms, and only about 1 ms of it goes away between native and 50%. That fits the structure above: the animation,
the refits and the top-level build do not depend on the resolution; only the two ray dispatches and the denoiser do.
Preset M at 50% costs 0.63 ms more than K (the creation-size fix works in game).

### Boiling grass with upscaling (2026-10-10)

With ray tracing on, the grass boiled at 50% and 67% but not at DLAA. Cause: the jitter cycle, not the resolution.
At 50% (ray tracing Low, then Max): 8 and 16 phases calm, 18 calm on Max, 32 boils strongly on Low and a little on
Max; 18 phases at 67% boiled on Low. The ray traced shadows start from the jittered G-buffer positions, so on grass
every jitter position gives a different result. The shadow denoiser keeps 90% history (about 10 frames): with up to
about 16 positions its window covers nearly all of them and the average holds still; with 32 each window sees a
different subset and the average drifts. Fewer rays (Low) mean more variance per position. The denoiser's constants
have no jitter or frame index, so keeping the game's 8-step counter would not help. Static detail shows the dot
pattern at 16 phases, so no cycle length suits both.

Candidate fixes: a longer denoiser history while the jitter cycle is long (feedbackWeight, editable per dispatch like
the post-processing constants: dev remote `rtfeedback <weight>`; risk: shadows lagging behind moving objects), or
Ray Reconstruction (replaces the denoiser and handles jittered noisy input by design).

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

1. Done: measured ray tracing as it is (frame times above, two RenderDoc captures). The rays follow the render scale;
   most of the cost does not.
2. Next session, dev build with the ray tracing switches (`OnDispatch` / `OnBuildAccelerationStructure` /
   `OnDispatchRays` in `src/addon.cpp`; remote `rtinfo` reports per-frame counts):
   - Cost split with PresentMon: `rtskip 1` (animation dispatches), `rtskip 3` (and the refits), `rtskip 4` (the ray
     dispatches), against `rtskip 0`. Skipped animation should not change the rasterized image (it only feeds the
     acceleration structures); if characters or trees freeze on screen, that assumption is wrong.
   - Amortized refits: `rtrefit 2`, `3`, `4` refit each object every Nth frame (round robin; its animation dispatches
     are skipped with it). Shadows of swaying trees and moving characters lag by up to N-1 frames; check how visible
     that is and what it saves.
   - Boiling: `rtfeedback 0.95` / `0.97` with the 32-phase jitter at 50%, on Low and Max; check grass calm and
     shadow lag behind moving objects.
3. Then decide on RR with numbers: its cost on this GPU (above) against the game's denoising cost and the boiling
   fix, and whether the game's sampling suits it.

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
