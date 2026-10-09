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
