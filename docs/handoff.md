# Handoff: Elden Ring DLSS, state on 2026-10-10

A ReShade 6.8 add-on (`src/`, built as `EldenRingDLSS.addon64`) loaded through me3 that replaces Elden Ring's TAA
with DLSS. Full history and measurements are in `docs/recon.md`; this file is the short version for picking up.

## Where things stand

| Stage | Status |
| --- | --- |
| 1. Recon | Done (2026-10-08) |
| 2. DLAA (DLSS at native resolution) | Done, in the everyday install |
| Wind motion vectors (grass, bushes, trees) | Done, in the everyday install |
| 3. Upscaling (render below native, DLSS upscales) | Works; in the everyday install at 50% with preset M |
| Ray tracing cost (distance-based BLAS refits) | Done, in the everyday install (halves the RT cost on Maximum) |
| DLSS Ray Reconstruction | Works in the dev install; image limited by the game's raw signal (see below) |
| 4. Release | Source published on GitHub (2026-10-10, at the user's request); no binaries |

The user chose NVIDIA's placement for the upscale (DLSS at the TAA, before tonemapping; "option 1") over upscaling
the tonemapped image at the final composite.

## Installs

Two me3 profiles under `%LOCALAPPDATA%\garyttierney\me3\config\profiles`:

- **Everyday** (`eldenring-default`, Start menu "ELDEN RING (me3)"): UnlockTheFps plus
  `natives\EldenRingDLSS` (release DLSS DLL). Build of commit 91d6b47, installed 2026-10-10 late evening with the
  user's OK: 50% with preset M (the user's choice; NVIDIA's default for Performance), DLSS jitter from the TAA
  filter's counter (the scene constants were a frame stale in 2-3% of frames, which made M and L judder), the
  distortion/sparks/grace-flame fixes, the distance-based ray tracing refits. Its ini: `Preset=13`,
  `RenderScale=0.50`, game sharpening off, game exposure on, chromatic aberration off, `WindMotion=1`, `WindMode=1`,
  `WindTrees=1`. Backups in `build\everyday-backup\`: b2ac485 (58%, K), 7182d94 (67%, K), f0e8c12 (DLAA). Only
  update it with the user's explicit OK and with the game closed (`tools/deploy.ps1 -Target main`).
- **Dev** (`eldenring-dlss-dev`, `natives\EldenRingDLSS-dev`, developer DLSS DLLs including `nvngx_dlssd.dll`):
  test builds only. Latest build (2026-10-10 night): DLSS Ray Reconstruction (`RayReconstruction=1` in its ini, used
  only while the game's ray tracing runs), raw shadows into RR, plus all ray tracing switches (`rr`, `rrraw`,
  `rrpreset`, `rrdump`, `rtsamples`, `rtrandom`, `rtskip`, `rtrefit`, `rtdist`, `rtinfo`, `jitter`, `jittersrc`,
  `dual`, `dualinfo`, `sceneinfo`). Ini: `RenderScale=0.50`, `Preset=13` (M), `GameExposure=1`. F9 toggles DLSS and
  saves `Enabled` to the ini: check `Enabled=1` (and the "Loaded. DLSS on" log line) before measuring.

## Working rules (from the user)

- Ask the user to be at the PC before launching the game, and wait for "ready" (loaded in, controls released)
  before in-game tests. Never load into the world without them (their character could be attacked).
- HDR: the vanilla game starts in SDR and switches to HDR at the first main-menu click. ReShade.log's output line
  never sees the switch, so don't conclude HDR is off from it. RenderDoc sessions do save HDR off; remind the user to
  turn it back on after one. A "Game output: 2560x1600 ... SDR" line means the monitor is off or the user is remote.
- If a launch hangs before the game creates its device (no "Redirecting D3D12CreateDevice" in ReShade.log, process
  not responding) and a relaunch fails with "Steam is required", Steam still lists the game as running: the user
  restarts Steam (Steam > Exit), then launch again (happened once on 2026-10-10).
- Every normal exit ends in a WER crash report (int3 at `eldenring.exe+0xC58DB6`, a shutdown allocator check). It
  predates this work and is harmless. A session that ends without that report was a real crash.

## Everyday workflow

```
pwsh -NoProfile -File build.ps1                       # VS + CMake + Ninja; shaders via fxc (DXBC) and dxc (DXIL)
pwsh -NoProfile -File tools/deploy.ps1 -Target dev    # use pwsh and absolute paths; game must be closed
"$env:LOCALAPPDATA\Programs\garyttierney\me3\bin\me3.exe" launch --auto-detect -p eldenring-dlss-dev
```

- Dev remote control: write a command to `remote.txt` in the dev folder; the answer appears in `remote.done`.
  `tools/remote.ps1 "<command>"` sends one; `tools/ab_capture.ps1 -Round <name>` runs scripted rounds (with
  PresentMon frame-time logs). Commands include `dlss`, `preset`, `mv N name` (TAA input dumps), `capture`,
  `wind`, `windtrees`, `windstats`, `windmerge drawn|merged`, `treedump`, `drsptr`, `drsstatus`, and for
  upscaling `scale <x>`, `ctx`, `passlog`, `postfull <mask>|off`, `upscaleat taa|composite`, `mvbasis`.
- `tools/mem.py` reads, searches and writes the running game's memory from outside (`read [s:1447F33E0]+EE0 30`,
  `write s:... <hex>`, `scanvt <static vtable>`); works under RenderDoc too, where the add-on is not loaded.
- Analysis scripts in `tools/` (Python venv at `.tools\venv`): `mv_check.py` (motion dumps; `--compare` finds
  pixels that change without motion vectors), `shots.py`, `wind_check.py`, `tree_check.py`, `prepass_check.py`.
- RenderDoc 1.46 is in `.tools\renderdoc`, launched through the `eldenring-recon` me3 profile (user presses F12).
  Captures are in `captures/`, the newest `er117_scale67_f2898.rdc` / `er117_scale100_f4572.rdc` (same spot, engine
  scale 0.67 and 1). Scripts: `qrenderdoc.exe --python tools/rdc_*.py` with `ER_*` environment variables (each
  script documents them); `rdc_passes.py` lists the post-processing chain with viewports and percentage constants.

## Wind motion vectors (done)

- Grass family (`M[AMSN_V]_Grass2`, `_Vesitation`, `_Grass2_LOD1`): `src/wind_port.*` / `wind_port.hlsl`, a port
  of the game's wind vertex shader drawn after each depth-writing grass draw into side targets, merged into the TAA
  motion texture (`wind_merge.hlsl`). About +0.8-1.1 ms at 4K in a grass field.
- SpeedTree trees and shrubs: the stream-output path (`src/wind.*`) replays the game's own tree shader for the
  previous pose with last frame's per-tree wind clocks, rebuilt on the GPU from two hash tables keyed by tree
  position (`tree_wind.hlsl`). About +1.1 ms at 4K at a forest edge. Overlay checkbox "Tree motion vectors".
- Not covered: the golden `WorldTree_Leaves` trees (they barely move) and the sky.
- Experiments kept off by default: `WindPrepass=1` (replacing the grass prepass pixel shader, correct but slower),
  `WindMode=0` (stream output for everything).

## Stage 3: upscaling (in the everyday install)

Details and measurements: `docs/recon.md`, "Stage 3" rounds 1-4 (addresses for `eldenring.exe` 2.7.1.0).

How it works (`RenderScale` in the ini, overlay slider, remote `scale`; dev install at 0.67):
- The engine's console dynamic resolution is live on PC. `src/render_scale.*` holds its scale
  (`[0x1447F33E0]+0xF04`, found by a code pattern) while DLSS upscales; the scene renders into the top-left of the
  full-size targets (2572x1448 at 0.67) and CB_AA's `g_vScreenPercentage` gives the share.
- DLSS runs at the TAA (NVIDIA's placement: before tonemapping, HDR input, game exposure) on that sub-rectangle.
- The passes after the TAA were queued by the engine at the render size. `StretchPostInputs` scales the five
  render-size inputs they read to full size (`src/stretch.*`: depth buffer and copy, half-size depth, emissive,
  scene motion; nearest, in place). `CorrectPostPass` fixes each draw as it is recorded: full-size viewport and
  scissor, and its resolution constants in the upload heap (list from the 67%/100% capture diff). Which command
  lists to correct is learned from submission order (lists after the TAA's, in frames with a TAA).
- `src/taa_jitter.*` lengthens the TAA's jitter cycle while upscaling (8 -> 8 x (1/scale)^2 = 18 at 0.67);
  without it static detail shows a dot pattern.
- 4K, 67%: 9.0 ms vs 11.0 ms DLAA vs 9.4 ms game TAA. User check: static detail, motion, effects, menus, map fine.
- The barrier listener (states for the stretch) is only registered when the game starts with a scale below 1;
  changing the scale from 100% to lower at runtime therefore needs a restart for the stretch (to do: handle).
- Development options: `upscaleat composite` (DLSS on the tonemapped image at the final composite, an earlier
  approach), `postfix off`, `passlog`, `postinfo`, `mvbasis`. Dead experiments still in the code: `drs_probe.*`,
  `engine_resolution.*`, `hw_breakpoints.*` (the last one found the jitter setting's writer).
- Version-specific: the jitter filter's vtable address (`0x142F242B8`) is hard-coded; the scale uses a pattern.

### Next steps

1. Preset M fix: confirmed in game (2026-10-10): 50% M costs 0.63 ms more than K.
2. The user's bigger goal: make ray tracing affordable. `docs/raytracing.md` has the measurements, how the game's
   ray tracing works (captures `er117_rtmax_*`), the boiling-grass cause (long jitter cycle against the shadow
   denoiser's 10-frame history) and the next session's tests with the dev build's switches (cost split, amortized
   refits, denoiser history). The everyday install also boils with ray tracing on (67%, 18 phases, Low).
2b. Done (2026-10-10 evening): the world judder at a grace was a one-frame-stale jitter that M and L show and K
   smooths over; DLSS now takes the jitter from the TAA filter's counter (remote `jitterinfo`). 58% needed a
   render-size tolerance (the engine rounds the width to 8 pixels with ray tracing on); list kinds seen on both
   sides of the TAA stay decided at submission (GPU predicates). Details in docs/recon.md.
2c. Ray Reconstruction (dev only, 2026-10-10): works end to end (guides from the G-buffer at the sun light pass, raw
   shadows, automatic switch with ray tracing), +1.9 ms over DLSS M at 50%; keeps grass shadows and ghosts less than
   the game's denoiser, but at 50% the grass shimmers because the game's raw signal is thin (AO at half size, few
   rays). **Next session:** replace the shadow ray generation shader with one that also traces full-size AO
   (docs/raytracing.md, "Next: replace the shadow ray generation shader"). The everyday install keeps DLSS M.
3. Hear how the everyday install at 67% plays over longer sessions and other areas.
4. Performance: the stretch passes and the full-size post-processing cost about 1 ms; check what is worth saving.
5. Robustness: scale changes at runtime (barrier listener), resolution changes (filter and target recreation), the
   first frame of a new command-list kind (not yet corrected), and other scale values (0.5, 0.58, 0.77).
6. Cleanup before a release: remove the dead experiments, overlay text for the render scale and jitter.

## Recent commits

See `git log`; the latest work (2026-10-10): DLSS jitter from the TAA filter's counter (91d6b47), list kinds
decided at submission with GPU predicates (c42f3de, b2ac485), the fire and grace fixes (67f97f5), Ray
Reconstruction (1219002 and later), raw AO and rays-per-pixel switches (2a6b4a6, 7b79d8d).
