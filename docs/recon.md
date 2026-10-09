# Stage 1 recon: ELDEN RING's TAA inputs

Capture: game App Ver. 1.17.1 (`eldenring.exe` 2.7.1.0), 3840x2160, exclusive fullscreen, Antialiasing HIGH,
motion blur off, ray tracing off. One RenderDoc 1.46 frame (`er117_openworld_f17976.rdc`, mine tunnel, player
running). Analysis scripts live in `tools/` (`rdc_survey.py`, `rdc_inspect.py`, `rdc_events.py`,
`rdc_bufdump.py`, `survey_query.py`, `jitter_check.py`, `viz_capture.py`).

**Verdict: go.** Everything DLSS needs exists in the frame and the game's shaders keep their resource names.

## Pass order around TAA (draws are full-screen triangles)

| Event | Shader (debug name) | Reads | Writes |
| --- | --- | --- | --- |
| 56953 | `MotionBlurWriteCMBVelocityPS` | `g_DepthTex` (depth copy) | camera motion into `MoveTex` (R16G16_FLOAT) |
| 56975-57340 | per-object velocity draws (skinned/moving meshes) | | object motion into the same `MoveTex`, depth-tested against the main depth |
| 57539 | motion/stencil prep | `g_DepthTex`, `g_MoveTex`, `g_StencilTexForCopy` (main depth) | stencil copy (R8_UINT) + `g_MoveTex` for TAA (R16G16_FLOAT) |
| 57551 | `TempAAPS` (`PostProcess\TempAA[StencilMask]`) | `SourceTex`, `PrevFrameTex`, `g_MoveTex`, `g_PrevMoveTex`, `g_StencilTex`, `g_PrevStencilTex` | TAA result (R11G11B10_FLOAT) |
| 57569 | `SharpenFilterPS` | TAA result | sharpened image back into the scene color (`SourceTex` resource) |

Replacing TAA = at the `TempAAPS` draw, skip the draw and write DLSS output into its render target. The rest of
the frame (sharpen, post-processing, UI) continues unchanged.

## DLSS inputs

- **Color**: `SourceTex`, R11G11B10_FLOAT, linear HDR radiance, not pre-exposed
  (`cbSceneParam.SC_PreExposure` = 1, `SC_SceneExposure` = 12.56 in this frame). Use HDR + auto-exposure, or
  feed `SC_SceneExposure` as the exposure value.
- **Depth**: main depth (D32_FLOAT_S8X24 typeless), reverse-Z (`FC_MatrixProj`: z = -5.0e-6 + 0.0500673 / w,
  near about 0.05). Set the depth-inverted flag. A second depth copy (`g_DepthTex`) matches on 98.3% of pixels.
- **Motion vectors**: `g_MoveTex`, R16G16_FLOAT at full resolution. Encoding (from `TempAAPS` and the camera
  pass): `stored = (cur_uv - prev_uv) * (16/9, 1)`, i.e. both axes in units of screen height; TAA decodes
  `prev_uv = uv - stored * g_vOrignalVelocityEncode.xy` with `.xy` = (0.5625, 1). In pixels:
  `prev - cur = -(stored.x * 0.5625 * W, stored.y * H)`, so the DLSS MV scale is (-2160, -2160) at 4K.
  Typical values this frame: median 21 px, p99 62 px.
- **Jitter**: Halton(2,3) sub-pixel offsets in the projection matrix. `cbSceneParam.FC_MatrixProj` (byte 256)
  is stored column-major; NDC jitter x at byte **264**, y at byte **280** (-0.000130208, -0.000154321 here =
  (-0.25, +0.1667) px with x right / y down). The render view-projection is jittered.
- **Motion vectors exclude jitter**: the previous-frame view-projection is built with the current frame's
  jitter, so the camera reprojection matrix (`g_mCurToPrevScreen`) cancels it (fit error 1.5e-5, the print
  precision). A still camera on a still scene gives zero motion. Do not set the "MV jittered" flag.

## Resource states at the TAA draw

All shader inputs use `D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE` (PS + non-PS), which compute can read. The TAA
render target is in `RENDER_TARGET`. The main depth stays readable until the barrier right after the
sharpen-pass setup, which moves it back to `DEPTH_WRITE`. SRVs and CBVs are bound with
`SetGraphicsRootDescriptorTable`.

## Dynamic resolution (stage 3)

The engine carries dynamic-resolution parameters everywhere: `cbPostProcessCommon.g_dynamicScreenPercentage*`,
`CB_AA.g_vScreenPercentage*` and `cbSceneParam.SC_DynamicResolution_ScreenPercentage` (byte 1376),
`SC_ScreenResolutionScale` (byte 1420), all 1.0 on PC. TAA already handles a history at a different screen
percentage, so forcing a lower percentage and letting DLSS upscale looks feasible.

## Other constants worth knowing

- `CB_AA` (TAA): `g_SharpenIntensity` 0.25 (used by the sharpen pass), `g_PrevRelativeExposure` 1,
  feedback weights.
- `cbSceneParam` (2048 bytes, register b8 in the object-velocity VS): `FC_ScreenSize` @16,
  `FC_MatrixProj` @256, `VC_MatrixViewProj` @384, `VC_PrevMatrixViewProj` @464, `FC_VelocityEncodeParam`
  @1120, `SC_SceneExposure` @1152, `SC_PreExposure` @1160, `SC_BackBufferSize` @1360.

## Stage 2 result (2026-10-08)

The prototype add-on (ReShade 6.8.0 loaded as `ReShade64.asi` through the `eldenring-dlss-dev` me3 profile, next
to UnlockTheFps) detected the TAA inputs in game and ran DLSS 310.9.1 DLAA at 3840x2160 with no evaluation
failures. Toggling with F9 showed less noise than the game's TAA. Jitter from `FC_MatrixProj` with the
"projection offset in pixels, y down" convention and motion scale (-H, -H) gave a stable image (no smearing
reported).

## Polish result (2026-10-08)

With the game in HDR, DLAA with the game's sharpening pass skipped looked clearly best (grass blades much
cleaner; the sharpening filter's artifacts stand out in HDR). In SDR the unsharpened DLAA reads as softer.
Feeding DLSS the tonemapper exposure instead of auto-exposure made a small difference (some highlights
popped more). Defaults: game sharpening off, game exposure on.

## Sharpness A/B (2026-10-09)

The game's samplers have no mip bias, so DLAA sampled textures as blurry as the engine's own TAA expects. A -1.0
bias (NVIDIA's value for DLAA) on the anisotropic and wrapping samplers was a "noticeable improvement" in game.

The user still found grass "noisy with TAA + sharpening, a little blurry with DLAA + no sharpening". Shots taken
through the add-on's remote control (`tools/ab_capture.ps1`, `tools/shots.py`, SDR, standing still in a Limgrave
field) with RMS detail per feature size:

| region (640x360 crop) | variant | 1 px | 2 px | 4 px | 8 px |
| --- | --- | --- | --- | --- | --- |
| midground grass | TAA + sharpen 0.25 | 20.9 | 19.5 | 16.5 | 12.5 |
| | DLAA | 10.1 | 13.4 | 14.0 | 12.0 |
| | DLAA + sharpen 0.10 | 12.2 | 14.8 | 14.8 | 12.0 |
| | DLAA + sharpen 0.25 | 14.7 | 16.5 | 15.5 | 12.3 |
| distant trees | TAA + sharpen 0.25 | 14.4 | 12.2 | 10.2 | 9.6 |
| | DLAA | 6.3 | 7.9 | 8.7 | 9.0 |
| | DLAA + sharpen 0.25 | 9.7 | 10.0 | 9.5 | 9.1 |
| stone ruin | TAA + sharpen 0.25 | 18.0 | 17.0 | 15.1 | 14.4 |
| | DLAA | 12.1 | 13.6 | 13.9 | 14.0 |
| | DLAA + sharpen 0.25 | 18.3 | 16.7 | 14.9 | 14.2 |

- Brightness and everything at 8 px and up match, so DLSS doesn't change the image's tone or shapes.
- On stone, DLAA plus the game's sharpening at its own strength (0.25) equals TAA, so the sharpen pass is the only
  difference there.
- On vegetation, TAA keeps 1.5-2x more 1-2 px detail even against sharpened DLAA, and its pixel noise (Laplacian
  energy) is 4-9x DLAA's. Most of that is the shimmer the user calls noise; it also reads as the grass's crunchy
  texture.
- Preset J measured the same as K.
- Untested so far: presets L/M (NVIDIA: sharper and less ghosting than K, but they cost more and always use
  auto-exposure). Another open question is whether wind-animated vegetation writes motion vectors. NVIDIA's guide
  names animated foliage as the most common place where motion vectors go missing.
- In game the user found preset M about 30 FPS slower (~70 FPS in the field) with more flicker in trees, and chose
  preset K without sharpening.

## Chromatic aberration (2026-10-09)

The tonemapping constants (`cbToneMap`, 512 bytes) include `g_vChromaticAberrationRG` (byte 224),
`g_vChromaticAberrationB` (byte 240) and `g_vChromaticAberrationShapeParam` (byte 320). The dungeon capture had
RG (0, 0, -0.00075, -0.00015), B (-0.0015, -0.0003) and shape (1.778, 1, 2, -1.5). Its
`ToneMap_TabledToneMapLUTPS` variant samples the scene once, so the offsets are used elsewhere, probably in
`ToneMap_PostOETFPS` (the pass after it, which also binds `cbToneMap`).

`tools/ca_measure.py` on the Limgrave shots shows the effect in game: red and blue shift horizontally in opposite
directions against green, about 0.8-0.95 px at the left and right edges, 0.2-0.5 px a quarter of the way in, and 0
at the center. There is no vertical shift. TAA and DLAA measure the same, because it runs after anti-aliasing. In
brightness terms it is a slight sideways smear at the screen sides.

The add-on's `ChromaticAberration=0` (overlay checkbox) zeroes both offset vectors in every `ToneMap_*` pass. It finds
the buffer by `g_vScreenSize` (byte 336: width, height, 1/width, 1/height).

In-game test (`ab_capture.ps1 -Round ca`, Limgrave, SDR): the add-on found `cbToneMap` at b1 with open-world offsets
RG (0, 0, -0.0002, 0) and B (-0.0004, 0), i.e. green and blue shifted horizontally only. With the offsets zeroed,
`ca_measure.py` reads 0.00 px in every column. Sharpness (mean gradient) rose 29% on the stone ruin at the right
edge and 13% on leaves at the left edge. The center was unchanged apart from grass moving in the wind.

## Stage 3 findings so far (2026-10-08)

- Strings in `eldenring.exe` show the engine's dynamic-resolution plumbing: the system property
  `Performance.IsEnableDynamicResolution` (next to `Performance.IsEnableLoadBalancer_GX`, in the
  `CSSystemProperties` block), the `CSGxLoadBalancerHelper` singleton, and a `ToneMapDynamicResolutionVS` shader.
- `LoadBalancerParam` (80-byte `LOAD_BALANCER_PARAM_ST` rows, layout from libER/Paramdex) holds the console
  dynamic-resolution settings: `dynamicResolution` (byte 42, load level at which it starts) and
  `dynamicResolutionPercentageMin/Max` (bytes 48/49). The game has 16 rows (e.g. 70-100% at 30 FPS, 70-100% at
  60 FPS, 40-75%). `tools/param_patch.py` finds the table in the running game by its type name and patches it.
- Test 1 (RenderDoc capture `er117_drs67_f12837`): property on, all rows patched to level 0 and 67-67% while in
  game. Every screen percentage at the TAA stayed 1.0; viewport 3840x2160.
- Test 2 (dev profile, add-on logging): properties `IsEnableDynamicResolution` and `IsEnableLoadBalancer_GX` on,
  params patched at the title screen before loading. Scene still at 100% and a 3840x2160 viewport. The PC build
  does not drive dynamic resolution from these.
- The CPU side: the function around `0x141A34560` fills the CPU copy of `cbSceneParam` from a per-view structure
  (array of 0x120-byte entries at `[r12]`). `SC_DynamicResolution_ScreenPercentage` comes from view+0xF4..0x100
  and the secondary percentage from view+0x104 onward; view+0xA0/0xA4 feed `FC_ProjectionType`/`FC_SsrMipScale`.
  `0x141B68C81` then copies the struct field by field into the mapped constant buffer.
- Next: find who writes view+0xF4 (and whether scene viewports follow it), then force it from the add-on.
- Note: under RenderDoc the game reports HDR as unsupported and saves HDR off; turn it back on after captures.
- One level further: the per-frame view setup around `0x140A3AD20` fills view+0xD4..0xF0 from a render context
  (`r13`, fields +0x964/+0x980/+0x994..0x9A8) and copies view+0xF4 = context+0x9AC (16 bytes, primary
  percentages) and view+0x104 = context+0x9BC (secondary). It also fills view+0x114.. from calls to
  `0x14025FA70`/`0x14025FA40` (render target sizes per index). No direct SIMD or float store to context+0x9AC
  exists in the code, so it is filled indirectly (bulk copy or through a pointer). Next: capture `r13` at
  runtime (hook or hardware breakpoint at `0x140A3ADBE`), write 0.67 there, and check whether the scene
  viewports follow (the add-on logs "Scene: screen percentage ..., geometry viewport ...").

## Wind-blown vegetation has no motion vectors (2026-10-09)

Test: dev remote command `mv 8 <name>` (`tools/ab_capture.ps1 -Round mv`) copies the TAA's `g_MoveTex` and
`SourceTex` of 8 consecutive frames into readback buffers and saves them to `shots/`; `tools/mv_check.py` analyzes
them. Limgrave grass field, grass and trees swaying in the wind, player idle, camera at rest, DLAA preset K.

- Only the player character carries motion: torso, cape and sword (idle breathing, 0.1-0.3 px). Grass, bushes and
  trees read exactly 0 in every frame (3.7% of the screen moves at all, all of it the character).
- The vegetation did move. Between two dumps 1 s apart (each averaged over its frames to cancel the jitter) the
  display brightness changed by 5.6-6.2% on grass and tree canopies against 0.7-2.1% on the static rock and
  ruin. Frame-to-frame change, which includes the jitter, is 2.6-4.6% everywhere.

| region | 1-frame change | 1-second change | pixels with motion |
| --- | --- | --- | --- |
| grass, lower left | 4.6% | 6.1% | 0.0% |
| grass, right | 4.3% | 5.6% | 0.0% |
| tree canopy | 4.3% | 6.2% | 0.0% |
| rock, center | 2.6% | 0.7% | 0.0% |
| stone ruin | 4.3% | 2.1% | 0.0% |
| character torso | 6.2% | 8.5% | 87.6% |

So the wind is a vertex-shader effect without a velocity output. DLSS reprojects swaying blades as if they
were static and blends their positions over time. That fits the user's "vegetation a little blurry" with DLAA.

How the game's TAA copes: `TempAA[StencilMask]` reads `g_StencilTex` (R8_UINT copy of the stencil). For motion
under 1.5 px its history factor is `w = a[s & 3] + (1 - a[s & 3]) * b[(s >> 2) & 3]` from
`CB_AA.g_FeedbackWeightScale[4]` (byte 160; open world: a = .x = 0.9, 0.85, 0.85, 0.8 and b = .y = 0, 0.2, 0.4,
0.8). For larger motion it rejects history when the low two stencil bits of the current and previous pixel differ.
Not known yet: which stencil class grass and trees get. If they get a class of their own, that is a vegetation mask.

### Stencil classes (2026-10-09)

The dump also saves `g_StencilTex` (R8_UINT); `tools/stencil_check.py` maps each value. Two Limgrave views (a grass
field at night, then close to a ruin wall):

| value | low bits / next bits | covers | TAA history factor `a + (1 - a) * b` |
| --- | --- | --- | --- |
| 0 | 0 / 0 | terrain, the ruin wall, wooden posts | 0.90 |
| 1 | 1 / 0 | the player character | 0.85 |
| 3 | 3 / 0 | grass, bushes, trees, natural rock humps | 0.80 |
| 4, 8 | 0 / 1, 0 / 2 | distant terrain and a far tree | 0.92, 0.94 |
| 7, 11 | 3 / 1, 3 / 2 | the same class-3 kinds of objects farther away | 0.84, 0.88 |
| 12 | 0 / 3 | sky | 0.98 |

The low two bits are an object class; the next two look like distance bands (more history further away). Class 3 is
"vegetation and natural rocks" rather than vegetation alone (the close ruin wall is class 0). It lines up with the
wind motion. In the ruin view, class 3 (58% of the screen) had 33% of its pixels change by more than 6% over one
second, with motion vectors on 0.1% of them. Class 0 (38%) changed on 1.6%. The game's TAA handles this by
giving class 3 the least history (0.80 against 0.90 for terrain).

### DLSS bias-current-color mask: no effect (2026-10-09)

The add-on can pass class 3 to DLSS as `pInBiasCurrentColorMask` (`src/vegetation_mask.hlsl` turns the stencil copy
into an R8_UNORM mask; ini `VegetationMask`, remote `vmask <strength>`, `vmaskall on` masks the whole screen).
The dumped mask was correct: 255 on every pixel in whole-screen mode, and 255 on exactly the class-3 pixels
otherwise (99% agreement with the same frame's stencil; the rest is one frame of wind). Captures at the ruin wall
(`ab_capture.ps1 -Round vmask` and `-Round vmaskpresets`) showed no effect with DLSS 310.x at DLAA. With the
whole screen masked at strength 1, the static wall changed by RMS 7.3-10.2 (x1000) for presets F, K, L and M.
Two unmasked captures differ by 6.8, so that is just capture noise. Fine noise on the wall stayed within ±5%. A
mask DLSS honored would have handed the wall the raw, aliased current frame. So this DLSS version ignores the input
for DLAA, even on the CNN preset F. Option 1 for the wind problem is out; the mask pipeline can be reused for a
vegetation-only post effect.

### Where the wind comes from (2026-10-09, capture `er117_drs67_f12837`)

`captures/inspect_veg` has the vertex shaders of one draw per vegetation material (`rdc_inspect.py`).

- Grass (`M[AMSN_V]_Grass2_Gbuf`, `_Grass2_LOD1_Gbuf`) and generic vegetation (`M[AMSN_V]_Vesitation_Gbuf`,
  `_Vesitation_Emissive_Glow_Gbuf`) share one vertex shader family. Wind time is
  `cbPerFrame.FC_GlobalTime` (b9, byte 432) plus a per-vertex phase (`TEXCOORD1.x` times a material float). Each
  instance lists up to 8 force sources (`cbInstanceData`, entry 7) in `StructuredBuffer<ForceParamStruct>
  g_forceParam` (t26, 24 KB; flags, position, direction, period, amplitude, gust pulse parameters). The vertex is
  displaced with sine waves and gust pulses, then weighted by a vertex attribute (`TEXCOORD2.z`).
- `M[AMSN][Sway]_Gbuf` and `M[WorldTree_Leaves]_Gbuf` also read `FC_GlobalTime` (no force buffer).
- All of them end the same way: `clip = (world - cbSceneParam.SC_CameraPos) * VC_MatrixViewProj` (b8). cbSceneParam
  also has `SC_PrevCameraPos` (byte 448) and `VC_PrevMatrixViewProj` (byte 464); cbPerFrame has `FC_GlobalDt`
  (byte 440).
- So the previous-frame position of any wind-animated vertex is the game's own vertex shader run with
  `FC_GlobalTime - FC_GlobalDt`, `SC_PrevCameraPos` and `VC_PrevMatrixViewProj` (ideally with last frame's
  `g_forceParam` as well). No velocity variant of these shaders is drawn.
- G-buffer vertex counts in that capture (index count x instances): Grass2 4.66 M, Vesitation 2.12 M, Grass2_LOD1
  1.07 M, WorldTree leaves 0.94 M, Sway 0.32 M. That's about 9 M per frame for the animated materials.

## Wind motion vectors: working for grass, visually negligible (2026-10-09)

`src/wind.*` (ini `WindMotion`, off by default; overlay checkbox "Wind motion vectors (experimental)"). For every
G-buffer draw of a wind-animated material (`Grass`, `Vesitation`, `SpeedTree`, `WorldTree_Leaves`, `[Sway]`), the
add-on replays the game's own vertex shader twice with stream output. The first pass uses the previous camera and
wind time; a compute shader builds those from the game's `cbSceneParam`/`cbPerFrame`, which live in GPU memory. The
second pass uses the current ones. At the TAA draw the streamed triangles are rasterized into `g_MoveTex` where
their depth matches the scene, in the game's own velocity encoding (from `C[DetailBlend]_Velo_Skin`).

What it took:
- The previous constants can't be read on the CPU, so `wind_patch.hlsl` copies and patches them on the GPU.
- Both stream buffers must stay bound for a whole command list, each in its own output slot. Re-binding per pass
  reloads the fill counters before earlier draws have written them back; appends get lost and the two streams drift
  apart, which made screen-sized garbage motion.

Result at a Limgrave grass field (`ab_capture.ps1 -Round winddiag`, `tools/wind_check.py`):
- Both streams hold 9.68 M vertices per frame and stay aligned. Near grass gets 0.7 px/frame of motion (92% of its
  pixels), mid grass 0.08 px. With a 0.5 s step the near grass reads 12 px, so the motion scales as it should.
- Trees (SpeedTree) get no motion: the resolve never writes on them. Not diagnosed (the depth may come from a
  `DptA` prepass that doesn't match the G-buffer replay).
- Cost at 4K: 9.0 -> 16.4 ms per frame (+5.8 ms for the two replays, +1.6 ms for the resolve).
- The user toggled it in game while looking at grass: "the difference is almost imperceptible". At ~0.7 px/frame of
  sway, DLSS's own history rejection already hides most of what the missing motion vectors cost. The softness on
  vegetation is DLAA's character, not missing motion.

Left in the code, off by default, for future use. Ideas if it's ever revisited: replace the game's draw with an
SO-enabled copy (saves one replay), early-Z in the resolve, skip far-LOD grass, fix the trees.
