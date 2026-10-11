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

Fixed in commit 41e83fa: the feature is created for the engine's render size. NVIDIA's table for the same case
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
2. Session 2 (2026-10-10, evening; note: DLSS was off by mistake for the first part, F9 had saved `Enabled=0`, so
   those runs were native resolution with the game's TAA, ray tracing Max, daytime):
   - Cost split (GPU busy, native TAA, RT Max): all on 20.3 ms; without the animation dispatches 19.5 (0.8 ms);
     without animation and refits 15.3 (refits 4.2 ms); without the ray dispatches 15.0 (rays and denoise 5.3 ms at
     native; less at 50%); without all three 10.5. The refits are the big resolution-independent part.
   - Skipping the animation dispatches leaves the rasterized movement normal (they only feed the ray tracing scene),
     but the stale copy of the player casts a large shadow onto the player when they move: characters must be refit
     every frame.
   - Refit every Nth frame (round robin, everything including characters): N=2 saves 2.5 ms, N=3 3.3 ms, N=4 3.7 ms
     (baseline 18.1-18.6 ms). User check at N=4: leaf shadows on a tree in strong wind look staccato next to the
     game's every-frame refits; N=2 is "a good middle ground"; grass looked fine. The user wants the distance-based
     schedule as well.
   - Boiling with DLSS at 50% (preset M, 32 phases), ray tracing Low: "extreme", far worse than the game's TAA at
     native (which boils somewhat on Low too). The `rtfeedback` edit did not apply (0 edits); its buffer is an upload
     heap, so the failure is elsewhere: the next build reports the step where `EditBoundConstants` stops (`rtinfo`).
3. Current dev build: characters (`_Skin` animation groups) always refit; vegetation by distance (`rtdist <near>
   <far>`, default 20 m / 50 m: nearer every frame, up to far every `rtrefit` frames (default 2), beyond every 2N,
   at most 8); distances from the last top-level build's instances against `SC_CameraPos` (instance transforms use the
   map's coordinates: the player was 3.75 m from the camera in the capture). The animation dispatches follow the
   interval their group had last frame. `rtdist 0` + `rtrefit 1` = the game's behavior. Next session: frame times and
   the user's look at near and far trees with the default schedule; `rtinfo` for the denoiser edit failure step;
   then the boiling test with a working `rtfeedback` on Low.
4. Then decide on RR with numbers: its cost on this GPU (above) against the game's denoising cost and the boiling
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

## Ray Reconstruction: first in-game results (2026-10-10, late)

Built (RayReconstruction=1, remote `rr`, `rrraw`, `rrpreset`, `rrdump`): guides drawn at the sun light pass from the
G-buffer (GBuffer0.xyz world normal, GBuffer1.x metalness, .y smoothness -> perceptual roughness
1 - 0.95 sqrt(smoothness), GBuffer2.rgb sRGB base color; specular albedo by NVIDIA's EnvBRDFApprox2 with the view
direction from cbLightAccParam), DLSS-RR evaluated at the TAA, only while DispatchRays run (else DLSS-SR). Raw input:
the shadow denoiser passes through (full size); the AO path (traced at half size, game denoiser, SSAO chain's
temporal pass, bilateral upscale) passes through with the temporal pass's history at 0 and a stochastic upscale
(ao_upscale.hlsl) so AO noise is per pixel. Dumped guides look right (world normals, textured albedo, metal
specular, sky defaults).

- Cost at 50% (RT Max, night): RR 18.2 ms vs DLSS M 16.4 ms (+1.9 ms). The game's denoiser costs ~0.13 ms.
- vs DLSS M with the game's denoiser: RR keeps the grass's ray traced shadows (the game's denoiser smooths them
  away) and ghosts less on swaying trees; preset F (default, "RR2") preferred over E (similar, F recovers more grass
  detail).
- Raw AO through the game's bilateral upscale was much noisier (2x2 blocks); with the stochastic upscale it is as
  quiet as keeping the game's AO denoiser, with a little more AO definition (default now `rrraw all`).
- Open: at 50% and 67% the grass stays shimmery and loses its blades ("running together"); at native (RR DLAA) it is
  clean. Not the refit schedule, not the jitter count (32 at 67% did not help). Suspect: the game's ray tracing
  noise is not fresh per frame (fixed pattern or a short cycle), which NVIDIA's guide names as the main pitfall.
  Next: extract the ray generation shader (DXIL library in CreateStateObject, capture er117_rtmax_*) and see how it
  draws random numbers; if from a short frame counter, feed it a free-running one.

## Ray Reconstruction: the raw signal (2026-10-10, night)

Findings from the session after "first in-game results" (dev build 7b79d8d and later):

- **The game's randomness is fine per frame.** Extracted the ray tracing shaders from the capture (DXIL libraries in
  `CreateStateObject`; `dxc -dumpbin`): ray generation `GXRayGen`, `GXRayGen_ao`, `GXRayGen_dirLightShadow`, miss
  and hit shaders, and per-material hit groups (SpeedTree, vegetation, ...) with any-hit shaders for alpha testing
  (so material lookups at hits exist, which matters for any future reflections). The shadow and AO ray generation
  shaders hash `frac(frac(sin(dot(pixel / size + frameRandom, (26.44, 48.24))) * 347.97) * 578.2)`; `frameRandom`
  (`g_sceneCB` byte 240) logged over 64 frames was a fresh value in [0, 1) every frame (`rtrandom log`). Not the
  cause.
- **Rays per pixel** (`l_rayGenCB`: `aoSampleCount`, `dirLightShadowSampleCount`; a local constant buffer reached
  through the descriptor table in the ray generation record, after the 32-byte shader identifier; `rtsamples <ao>
  <shadow>`): Maximum uses AO 4, shadows 2. With RR at 50%: shadows 4 "a little better" (+0.9 ms), 8 no better than
  4; raw AO with 8 or 16 rays still noisy (16: +3.7 ms more).
- **Why raw AO stays noisy:** the AO is traced at half size. The stochastic upscale turns the differences between the
  4 half-size neighbors into per-pixel noise; in grass (every blade at another depth) that noise is large no matter
  how clean each sample is. Feeding RR the game's fully denoised input is calm, so the noise is the raw signal, not
  RR. The game's denoiser looks calmer because it blurs (a-trous, 10-frame history without motion), losing the fine
  grass shadows; RR keeps detail and needs a denser signal to tell detail from noise (clean at native).
- Current default: RR with raw shadows (`rrraw shadows`), AO from the game's denoiser; shadow rays at the game's 2
  (4 is a reasonable option, +0.9 ms). Measured: RR +1.9 ms over DLSS M at 50%.

How games built for RR do it: every effect traced at the render size (1-2 rays per pixel), well-distributed samples
(blue noise, importance sampling) and sample reuse (ReSTIR), all inputs RR asks for, and performance from low
render scales and frame generation rather than many rays. Foliage is hard for RR everywhere.

## Our own shadow ray generation shader in the game's pipeline (2026-10-11)

The user's call (2026-10-10): stop tuning RR around the game's cheap signal and give it a proper one. First step done:
the add-on can run its own ray generation shader for the game's sun shadow dispatch, with the game's scene, hit
groups and miss shaders.

**The game's shaders** (`tools/rdc_rtlibs.py` dumps every state object's DXIL libraries from a capture; `dxc
-dumpbin` disassembles them). The ray tracing pipeline (one `CreateStateObject` of type RAYTRACING_PIPELINE) holds
`GXRayGen`, `GXRayGen_ao`, `GXRayGen_dirLightShadow`, shadow and radiance miss/closest-hit/any-hit shaders, hit
groups `Model[shadow|radiance]`, `Ignore[...]` and `Dummy`, and ~20 existing collections with the per-material hit
groups (each a collection with its own closest-hit and alpha-testing any-hit shader). The ray generation and miss
shaders share local root signature 6723 (`l_rayGenCB` b16, normal t67, depth t68); the global one (6722) has
`g_sceneCB` b0, the scene t64 and the output u0, all space 0; shader model lib_6_3, payload 12 bytes.
- `GXRayGen_dirLightShadow` (full render size): world position from the depth and `mtxInvProj`/`mtxCamera`; two
  random numbers from the pixel and `frameRandom` (the sin hash above); `dirLightShadowSampleCount` rays toward
  `lightDir` inside a cone (`dirLightShadowConeRadius`, sqrt-radius and van der Corput angle), origin pushed along
  the normal when `adjustRayStartPosition` is set, 1 ray and no cone beyond `dirLightShadowMaxDist`; flags accept
  first hit, skip closest hit, cull back faces (+ force opaque unless `enableShadowAlpha`), instance mask 2.
  Output: (fraction of rays that reach the sky, view depth as a half float split into two bytes, 0).
- `GXRayGen_ao` (half size; half-size depth, full-size normal at 2x the pixel): the same hash; `aoSampleCount`
  cosine-weighted rays around the normal (mask 1), up to `aoMaxDist`; output through `aoScaleOffset`, same layout.
- `l_rayGenCB` is one buffer for all three, so the AO settings are readable in the shadow dispatch.

**How ours gets in** (`src/rt_pipeline.*`, `src/rt_raygen.hlsl`):
- ReShade's `create_pipeline` event cannot change it: when an add-on edits a D3D12 state object, ReShade 6.8
  rebuilds it from its own description, which drops local root signatures (`case
  D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE: break;` in its d3d12_device.cpp).
- So the add-on hooks the native device's `CreateStateObject` (vtable slot 62, checked with `offsetof` on the SDK's
  C declaration) below ReShade, and appends to the game's pipeline description our DXIL library (exports
  `ErdlssShadowRayGen`, `ErdlssShadowAoRayGen`) and an association giving them the game's ray generation local root
  signature. The game's shaders stay; if creation fails, the game's description is used unchanged.
- At each `DispatchRays` whose ray generation record holds `GXRayGen_dirLightShadow`'s identifier, the add-on copies
  the record (identifier + the game's local root arguments) into its own upload ring with our identifier and issues
  the dispatch again with it. The game's shader table is never written (rewriting it could race a frame in flight).
- Remote `rtgen 0|1|2` (ini `RtShadowRayGen`, read only): the game's shader, our port, our port plus AO at the
  render size in the output's 4th channel.

**In game** (dev build, 4K at 50%, preset M, RR on, ray tracing Maximum, daytime):
- The game created its pipeline 8 times while loading; every time with our shaders added, no fallback.
- The port matches the game's shader: alternating screenshots differ by less than two frames of the same shader
  (12x12 area means, max 0.8 vs 2.4 levels); GPU 17.36 ms vs 17.37 ms.
- AO at the render size with the game's 4 rays per pixel: 23.7 ms (+6.4 ms; four times the game's AO rays).
- Our AO in the 4th channel does not change the image (alternating shots within frame-to-frame noise): the
  shadow denoiser and the composite do not visibly read it.

**Render-size AO into Ray Reconstruction** (same day, later):
- `rtgen 2` while RR runs (`OurAoActive`): our shader traces 1 AO ray per pixel at the render size (the game's
  `l_rayGenCB.aoSampleCount` set to 1 unless `rtsamples` says otherwise; the game's AO ray count was 4 at half size,
  the same number of rays), the game's half-size AO dispatch is skipped (its record holds `GXRayGen_ao`'s
  identifier), the render-size region of the shadow output (the dispatch's u0, found through the compute root
  signature's tables: 3840x2160 R8G8B8A8_UNORM) is copied right after our dispatch, and the AO upscale pass draws
  the copy's 4th channel into the game's AO target (`ao_upscale.hlsl` PSFull) instead of upscaling. Without RR,
  mode 2 acts as mode 1 (the game's AO path needs its own denoiser input).
- Cost: slightly less than the game's AO path: 15.74 vs 16.00 ms and 12.97 vs 13.17 ms GPU at two spots (4K, 50%,
  M, RR, ray tracing Maximum). Counters per frame: skipped = copied = drawn, no failures.
- First look (user): "the grass blades that used to be dark are darker now" (per-pixel contact occlusion the
  half-size AO blurred away), and a "mirage" wave in distant grass. Cause: the game's random numbers, sin(dot(pixel
  / size + frameRandom, (26.439, 48.236))) hashed, form diagonal bands that the per-frame offset sweeps across the
  screen; the game's denoisers hid them, RR kept them. With the game's AO the wave was gone. Mode 2 now uses PCG
  (per pixel, per frame from `frameRandom`, separate streams for shadow and AO rays); the exact port (mode 1) keeps
  the game's hash. User: "Yeah I think it looks great." Dev install default: `RtShadowRayGen=2`.

**Next:**
1. Blue noise or a low-discrepancy sequence per pixel instead of white noise (RR handles white noise; blue noise
   converges faster at 1 ray per pixel).
2. More sun shadow rays only where the cone is wide (penumbrae), or 1 ray with RR as for the AO; measure.
3. Without RR: a denoiser for our AO, or keep the game's path (current).
4. ReSTIR-style reuse matters once there are many lights to choose from (the local lights, e.g. torches, or GI);
   for the sun and AO, RR's own temporal accumulation does the reuse. The pipeline already has radiance miss and
   closest-hit shaders (`GXMiss_radiance`, `GXClosestHit_radiance`, `GXRayGen`), useful for reflections or GI.
