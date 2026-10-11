# Handoff: Elden Ring DLSS, state on 2026-10-11

A ReShade 6.8 add-on (`src/`, built as `EldenRingDLSS.addon64`) loaded through me3 that replaces Elden Ring's TAA
with DLSS. Full history and measurements are in `docs/recon.md` (rendering, upscaling, motion vectors) and
`docs/raytracing.md` (ray tracing, Ray Reconstruction, our ray generation shaders); this file is the short version
for picking up.

## Where things stand

| Stage | Status |
| --- | --- |
| 1. Recon | Done (2026-10-08) |
| 2. DLAA (DLSS at native resolution) | Done, in the everyday install |
| Wind motion vectors (grass, bushes, trees) | Done, in the everyday install |
| 3. Upscaling (render below native, DLSS upscales) | Works; in the everyday install at 50% with preset M |
| Ray tracing cost (distance-based BLAS refits) | Done, in the everyday install (halves the RT cost on Maximum) |
| DLSS Ray Reconstruction | Dev install; with our own ray generation shaders (below) |
| Own ray generation shaders (render-size AO, 1 ray, blue noise) | Dev install default (`RtShadowRayGen=3`) |
| 4. Release | v0.1.0 on GitHub (release zip) and Nexus Mods (2026-10-11) |
| Motion vectors for enemies and animals | **Open**: only the player writes them (next step 1) |
| Object-only motion blur | **Wanted** by the user (next step 2) |

The user chose NVIDIA's placement for the upscale (DLSS at the TAA, before tonemapping; "option 1") over upscaling
the tonemapped image at the final composite.

## Installs

Two me3 profiles under `%LOCALAPPDATA%\garyttierney\me3\config\profiles`:

- **Everyday** (`eldenring-default`, Start menu "ELDEN RING (me3)"): UnlockTheFps plus
  `natives\EldenRingDLSS` (release DLSS DLL). Build of commit 91d6b47, installed 2026-10-10 late evening with the
  user's OK: 50% with preset M, DLSS jitter from the TAA filter's counter, the distortion/sparks/grace-flame fixes,
  the distance-based ray tracing refits. No Ray Reconstruction. Its ini: `Preset=13`, `RenderScale=0.50`, game
  sharpening off, game exposure on, chromatic aberration off, `WindMotion=1`, `WindMode=1`, `WindTrees=1`. Backups
  in `build\everyday-backup\`: b2ac485 (58%, K), 7182d94 (67%, K), f0e8c12 (DLAA). Only update it with the user's
  explicit OK and with the game closed (`tools/deploy.ps1 -Target main`).
- **Dev** (`eldenring-dlss-dev`, `natives\EldenRingDLSS-dev`, developer DLSS DLLs including `nvngx_dlssd.dll`):
  test builds only. Latest build (2026-10-11, commit 56dbb23 and later): Ray Reconstruction (`RayReconstruction=1`,
  used only while the game's ray tracing runs) fed by our own sun shadow ray generation shader with render-size AO
  (`RtShadowRayGen=3`: blue noise, 1 AO and 1 shadow ray per pixel). Ini: `RenderScale=0.50`, `Preset=13` (M),
  `GameExposure=1`. F9 toggles DLSS and saves `Enabled` to the ini: check `Enabled=1` (and the "Loaded. DLSS on" log
  line) before measuring.
- A test copy of the release zip lives in `dist/test` (profile `dist/test/EldenRingDLSS.me3`, and
  `EldenRingDLSS-fps.me3` with UnlockTheFps for frame-rate measurements); not an install the user plays with.

The user's game settings: Custom, almost everything Maximum (water, shader and GI quality High), ray tracing
Maximum, motion blur off, 4K, HDR on. Game version: App Ver. 1.17.1 = `eldenring.exe` 2.7.1.0.

## Working rules (from the user)

- Ask the user to be at the PC before launching the game, and wait for "ready" (loaded in, controls released)
  before in-game tests. Never load into the world without them (their character could be attacked). For daylight,
  they can rest at a grace and "Pass time" to Noon.
- The user is fine with commits being pushed to GitHub without asking. Anything else outward-facing (Nexus,
  Reddit, forums, releases) is confirmed per action; the user posts with their own accounts.
- HDR: the vanilla game starts in SDR and switches to HDR at the first main-menu click. ReShade.log's output line
  never sees the switch, so don't conclude HDR is off from it. Remote `capture` shots carry the same stale tag (space 1
  in the .txt while the image is HDR10 PQ, e.g. the 2026-10-11 showcase shots); if a converted shot looks flat and
  grey, decode it as PQ (space 3). RenderDoc sessions do save HDR off; remind the user to turn it back on after one.
  A "Game output: 2560x1600 ... SDR" line means the monitor is off or the user is remote.
- If a launch hangs before the game creates its device (no "Redirecting D3D12CreateDevice" in ReShade.log, process
  not responding) and a relaunch fails with "Steam is required", Steam still lists the game as running: the user
  restarts Steam (Steam > Exit), then launch again (happened once on 2026-10-10).
- Every normal exit ends in a WER crash report (int3 at `eldenring.exe+0xC58DB6`, a shutdown allocator check). It
  predates this work and is harmless. Windows sometimes skips repeated identical reports, so a missing report is not
  proof of a crash: ask the user how the game closed (2026-10-11: a normal quit without a report).
- Remote command pitfalls: `dlss` without an argument switches DLSS **off** (it compares the argument with "on");
  `rr on|off` saves to the ini. Use `rtgen`, `rtsamples`, `scale`, `rrraw` without an argument to just read the state.

## Everyday workflow

```
pwsh -NoProfile -File build.ps1                       # VS + CMake + Ninja; shaders via fxc (DXBC) and dxc (DXIL)
pwsh -NoProfile -File tools/deploy.ps1 -Target dev    # use pwsh and absolute paths; game must be closed
"$env:LOCALAPPDATA\Programs\garyttierney\me3\bin\me3.exe" launch --auto-detect -p eldenring-dlss-dev
pwsh -NoProfile -File tools/package.ps1               # release zip in dist\ (version from CMakeLists.txt)
```

- Dev remote control: write a command to `remote.txt` in the dev folder; the answer appears in `remote.done`.
  `tools/remote.ps1 "<command>"` sends one; `tools/ab_capture.ps1 -Round <name>` runs scripted rounds (with
  PresentMon frame-time logs; `-Folder` points it at another install, e.g. `dist/test/EldenRingDLSS`; rounds
  `showcase`/`showcasert` made the README's screenshots and frame rates). Commands include `dlss`, `preset`,
  `mv N name` (TAA input dumps), `capture`, `scale <x>`, the wind and upscaling switches, and for ray tracing
  `rtgen [0-4]` (sun shadow ray generation: game's, port, port + render-size AO with PCG, blue noise, temporal blue
  noise), `rtsamples <ao> <shadow>` (0 = game's/default), `rr`, `rrraw none|shadows|all`, `rrpreset`, `rtinfo`.
- `tools/mem.py` reads, searches and writes the running game's memory from outside; works under RenderDoc too.
- Analysis scripts in `tools/` (Python venv at `.tools\venv`): `mv_check.py` (motion dumps), `shots.py`,
  `wind_check.py`, `tree_check.py`, `prepass_check.py`, `blue_noise.py` (regenerates `src/blue_noise.hlsli`).
- RenderDoc 1.46 is in `.tools\renderdoc`, launched through the `eldenring-recon` me3 profile (user presses F12;
  captures land in `%LOCALAPPDATA%\Temp\RenderDoc`, moved to `captures/`). Newest: `er117_npcs_f978.rdc` (a soldier
  and sheep moving, motion blur off; analysis in `captures/npcs/`), `er117_rtmax_*` (ray tracing; its DXIL libraries
  dumped to `captures/rtlibs/` by `tools/rdc_rtlibs.py`). Scripts: `qrenderdoc.exe --python tools/rdc_*.py` with
  `ER_*` environment variables (each script documents them); `rdc_thumb.py` saves a capture's preview image.

## Release and outreach

- GitHub release v0.1.0 (`EldenRingDLSS-0.1.0.zip`, SHA-256 `0AEBC833...C07A`): an me3 profile, the add-on, NVIDIA's
  release DLSS DLLs, a commented ini (DLAA, F9, wind motion vectors on), `INSTALL.txt`, licenses. ReShade is not
  included (users take `ReShade64.dll` from the installer with 7-Zip). Tested in game from the zip.
- Nexus Mods page 11315 (nexusmods.com/eldenring/mods/11315), published 2026-10-11 by the user: the same zip, mod
  manager download off (Vortex cannot set up me3), requirements me3 and ReShade (off-site), permissions: no
  re-upload, modification/conversion/asset use with credit, tags Performance Optimization, ReShade, AI-Generated
  Content; GitHub as a download mirror; no donation points (NVIDIA asks to be notified before commercial use).
- Not posted yet (drafts in `dist/outreach.md`, local): Reddit (r/EldenRingMods, r/nvidia), PCGamingWiki
  (Modifications entry and upscaling row), the ReShade forum.
- For the next release: the zip's `INSTALL.txt` still names only the exe's file version (the repo's says App Ver.
  1.17.1). The comparison slider pairs the game's TAA (sharpened) with unsharpened DLSS, captured 36 s apart while the
  light changed; a fairer pair would be back-to-back, possibly with `GameSharpening=1`.

## Ray tracing with our own ray generation shaders (dev)

Details: `docs/raytracing.md`, "Our own shadow ray generation shader in the game's pipeline".
- The add-on hooks the native device's `CreateStateObject` (vtable slot 62) below ReShade, because ReShade 6.8
  rebuilds edited state objects without their local root signatures, and appends `rt_raygen.hlsl`'s library plus an
  association to the game's ray generation local root signature. The sun shadow dispatch runs ours through a copy of
  its ray generation record in the add-on's upload ring; the game's shader table is never written.
- With RR (modes 2-4): 1 AO ray per pixel at the render size into the shadow output's unused 4th channel, copied
  after the dispatch and drawn into the game's AO target (`ao_upscale.hlsl` PSFull); the game's half-size AO dispatch
  is skipped; 1 sun shadow ray per pixel instead of 2. Cheaper than the game's path (about 1.6 ms at 4K/50%).
- The game's sin hash made a "mirage" wave in distant grass under RR; PCG or blue noise fixed it. Blue noise (mode
  3) is the default; temporal blue noise (mode 4, a frame index written into `g_sceneCB`'s padding) looked the same.
- Noise the user still sees in grass at 50% is the upscale, not the ray tracing (it stays with the game's AO, with
  the shadow denoiser on and with RR off; calmer at 67% and DLAA). The user keeps 50% for the frame rate (50% 58,
  58% 49, 67% 46 fps at that spot with RR).

## Next steps

1. **Motion vectors for enemies and animals** (docs/recon.md, "Other characters have no motion vectors"). Only the
   player's meshes get velocity draws: in `er117_npcs_f978.rdc` all 36 velocity draws (events 124272-124637) match
   the player's G-buffer draws (111520-111830); the NPC G-buffer draws (112034 on: soldier-size meshes 6x each,
   `C[Fur]` sheep 5x, more fur and shell meshes 3x) have none. The player's `*_Velo_Skin` vertex shaders bind
   `cbObjMatrix` and `cbPrevObjMatrix` (3 KB bone palettes) plus `cbClothCancelObjMatrix` and its previous copy; the
   NPC draws bind only the current `cbObjMatrix` (64 KB, several instances, picked through `cbInstanceData` /
   `cbInstanceIdxParam`), so the game keeps no previous pose for them. Turning the game's motion blur on does not
   add NPC velocity (tested 2026-10-11). Plan: keep a GPU copy of each NPC draw's bone palette every frame (keyed by
   vertex/index buffer, index range and instance; check that instance order is stable across frames), and draw
   their velocity with the game's own `C[...]_Velo_Skin` pipelines (captured at creation by shader debug name) with
   the current palette and our previous copy, depth-tested into the TAA's motion texture like the wind merge. Watch
   alpha-tested `[A]` variants and the cloth-cancel matrices. Alternative: find the per-character switch for
   velocity in the code (Ghidra) and turn it on for everyone.
2. **Object-only motion blur.** The user likes object motion blur but not camera motion blur; today the game's
   object blur only covers the player (no NPC velocity), and turning motion blur on blurs camera motion too. Plan: a
   capture with motion blur on to find the blur passes, then remove the camera part of the motion they read (the
   camera motion is what `MotionBlurWriteCMBVelocityPS` writes from depth and the matrices) or give them an
   object-only velocity. Check that our wind motion vectors (merged into the same motion texture) do not get grass
   blurred. Depends on step 1 for enemies.
3. RR's responsivity mask on vegetation (preset F only; negative values favor stability) for the grass at 50%.
4. Ray Reconstruction with our shaders in the everyday install: measure it side by side with the current DLSS M
   setup first; the user decides.
5. The outreach drafts above, one at a time with the user.
6. Older items: robustness (scale changes at runtime need a restart for the stretch; resolution changes; the first
   frame of a new command-list kind), performance of the stretch and full-size post-processing (about 1 ms), and
   removing dead experiments (`drs_probe.*`, `engine_resolution.*`, `hw_breakpoints.*`).

## Recent commits

See `git log`. 2026-10-11: release packaging and README (bf33a45, d20a54c, 28007e3, 3962d3e); our ray generation
shaders in the game's pipeline (6d7c0b1), render-size AO into RR and PCG noise (9c60c29), blue noise (23b8223),
temporal blue noise option (9ffc216), 1 sun shadow ray (56dbb23), the NPC motion vector finding (7883869).
