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
