# Handoff: Elden Ring DLSS, state on 2026-10-10

A ReShade 6.8 add-on (`src/`, built as `EldenRingDLSS.addon64`) loaded through me3 that replaces Elden Ring's TAA
with DLSS. Full history and measurements are in `docs/recon.md`; this file is the short version for picking up.

## Where things stand

| Stage | Status |
| --- | --- |
| 1. Recon | Done (2026-10-08) |
| 2. DLAA (DLSS at native resolution) | Done, in the everyday install |
| Wind motion vectors (grass, bushes, trees) | Done, in the everyday install |
| 3. Upscaling (render below native, DLSS upscales) | In progress: the scale works; full-size post-processing built, not yet run in game |
| 4. Release | Not started. Publish to GitHub only when the user says so |

The user chose NVIDIA's placement for the upscale (DLSS at the TAA, before tonemapping; "option 1") over upscaling
the tonemapped image at the final composite.

## Installs

Two me3 profiles under `%LOCALAPPDATA%\garyttierney\me3\config\profiles`:

- **Everyday** (`eldenring-default`, Start menu "ELDEN RING (me3)"): UnlockTheFps plus
  `natives\EldenRingDLSS` (release DLSS DLL). Build of commit 6e672fa, installed 2026-10-09. Its ini: preset 0 (K),
  game sharpening off, game exposure on, chromatic aberration off, `WindMotion=1`, `WindMode=1`, `WindTrees=1`.
  Only update it with the user's explicit OK and with the game closed (`tools/deploy.ps1 -Target main`).
- **Dev** (`eldenring-dlss-dev`, `natives\EldenRingDLSS-dev`, developer DLSS DLL): test builds only. It has the
  full-size post-processing build (2026-10-10, see "Stage 3" below), with `RenderScale=0.67` and `GameExposure=1`.

## Working rules (from the user)

- Ask the user to be at the PC before launching the game, and wait for "ready" (loaded in, controls released)
  before in-game tests. Never load into the world without them (their character could be attacked).
- HDR: the vanilla game starts in SDR and switches to HDR at the first main-menu click. ReShade.log's output line
  never sees the switch, so don't conclude HDR is off from it. RenderDoc sessions do save HDR off; remind the user to
  turn it back on after one. A "Game output: 2560x1600 ... SDR" line means the monitor is off or the user is remote.
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

## Stage 3: upscaling (in progress)

Details, addresses and measurements: `docs/recon.md`, "Stage 3" rounds 1-4 (addresses for `eldenring.exe` 2.7.1.0).

What works (in game, 2026-10-10):
- The engine's console dynamic resolution is live on PC. Its scale is a float at `[0x1447F33E0]+0xF04` (1.0 on PC);
  `src/render_scale.*` finds the object through a code pattern and the add-on holds the scale at `RenderScale`
  while DLSS upscales (ini, overlay slider "Render scale", remote `scale`). At 0.67 the scene renders at 2572x1448
  into the top-left of the full-size targets; CB_AA's `g_vScreenPercentage` gives the exact share.
- DLSS takes that sub-rectangle (Quality mode at 0.67; the feature is created for the mode's dynamic range).
  Jitter arrives in render pixels. 4K frame times: 8.1 ms at 67% vs 10.9 ms DLAA vs 9.6 ms game TAA.

The open problem: every pass after the TAA (particles, depth of field, glow, bloom, light shafts, tonemap) also runs
at the render size; on PC nothing upscales the result (the final composite `GXSimple_BlendUI` reads `HDRScene` one
to one), so the screen showed the top-left 67% of the image. The 67%/100% captures differ there only in viewports
and percentage constants.

Built but not yet run in game (the latest commit, in the dev install):
- DLSS at the TAA as for DLAA (NVIDIA's placement), then `src/stretch.*` scales up the five render-size inputs the
  later passes read (depth buffer, depth copy, half-size depth, emissive, the scene's motion; nearest, in place) and
  `src/post_resolution.*` switches the render context to full size for the passes recorded after the TAA, restored
  at the composite. The context is found on the recording thread's stack (no code hook).
- A first approach, upscaling the tonemapped `HDRScene` at the composite, stays as a development option
  (`upscaleat composite`); NVIDIA says DLSS must run before tonemapping, and LDR input is 8-bit.

Static analysis setup:
- Ghidra 12.1.4 + Temurin JDK 21, portable in `.tools\ghidra` and `.tools\jdk`. The analyzed project is in
  `Documents\GhidraProjects\EldenRing` (from `.tools\ghidra_input\eldenring.exe`). Queries:
  `pwsh tools/ghidra/query.ps1 <command> <args>` with `decompile`, `decompileat` (creates a missing function),
  `xrefs`, `calls`, `writes <hexdisp>`, `symbols <text>` (RTTI class names survive), `near`, `ptrs`, `bytepair`.
  Each query takes about a minute (it opens the project headless).
- Quick helpers: `tools/xref.py` (disassemble, RIP-relative references), `tools/funcs.py` (function ranges from
  .pdata, direct callers), `tools/find_stores.py`, `tools/find_disp.py` (instructions using a displacement).
- Hardware breakpoints (`src/hw_breakpoints.*`) crash the game after about 30 s (anti-tamper). Don't use them.

### Next steps, in order

1. **Run the test** (user in game outdoors, HDR on): launch the dev profile, then
   `pwsh tools/ab_capture.ps1 -Round upscale`. Check the log ("Full-size post-processing: N buffers stretched ...
   switched context ..."), `shots/passlog_up67.txt` (every draw after the TAA should have full-size viewports; compare
   `passlog_up67_noswitch.txt`), the screenshots (`tools/shots.py`) and frame times. Then the user looks around:
   sharpness, smearing, particles and depth of field.
2. If the context is not found on the stack (log warning after 30 misses): find the render contexts in the heap by
   the same signature (+0x448 == +0x7B0, see `tools/mem.py` scans in recon round 3) or hook the AA filter's per-frame
   function (`0x141AD79A0`, context = second argument).
3. If some passes stay at the render size: find where they read their size (Ghidra) and add it to the switch.
4. Then: performance (the stretch costs a few copies), image checks against DLAA, the overlay text, and the
   everyday install with the user's OK.

## Recent commits

```
0b84427 Stage 3: upscale at the final UI composite
c42575c Stage 3: render scale through the engine's dynamic resolution, DLSS sub-rect input
e643a54 Stage 3: find the dynamic-resolution scale, add an external memory tool
a3d552f Add a handoff note for the next session
19faa1b Stage 3 research: resolution-state probe, Ghidra queries, corrected notes
6e672fa Tree motion vectors: overlay checkbox, lazy start, indirect resolve   <- everyday install
```
