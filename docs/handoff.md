# Handoff: Elden Ring DLSS, state on 2026-10-09

A ReShade 6.8 add-on (`src/`, built as `EldenRingDLSS.addon64`) loaded through me3 that replaces Elden Ring's TAA
with DLSS. Full history and measurements are in `docs/recon.md`; this file is the short version for picking up.

## Where things stand

| Stage | Status |
| --- | --- |
| 1. Recon | Done (2026-10-08) |
| 2. DLAA (DLSS at native resolution) | Done, in the everyday install |
| Wind motion vectors (grass, bushes, trees) | Done, in the everyday install |
| 3. Upscaling (render below native, DLSS upscales) | In progress: research, next step is an in-game probe |
| 4. Release | Not started. Publish to GitHub only when the user says so |

The user wants to continue stage 3 and does not want to give up on it ("there's no way it's impossible to change
the resolution").

## Installs

Two me3 profiles under `%LOCALAPPDATA%\garyttierney\me3\config\profiles`:

- **Everyday** (`eldenring-default`, Start menu "ELDEN RING (me3)"): UnlockTheFps plus
  `natives\EldenRingDLSS` (release DLSS DLL). Build of commit 6e672fa, installed 2026-10-09. Its ini: preset 0 (K),
  game sharpening off, game exposure on, chromatic aberration off, `WindMotion=1`, `WindMode=1`, `WindTrees=1`.
  Only update it with the user's explicit OK and with the game closed (`tools/deploy.ps1 -Target main`).
- **Dev** (`eldenring-dlss-dev`, `natives\EldenRingDLSS-dev`, developer DLSS DLL): test builds only. It currently has
  the build of commit 19faa1b (the stage 3 probe build).

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
  `wind`, `windtrees`, `windstats`, `windmerge drawn|merged`, `treedump`, `drs`, `drsptr`, `drsprobe`,
  `drsstatus`.
- Analysis scripts in `tools/` (Python venv at `.tools\venv`): `mv_check.py` (motion dumps; `--compare` finds
  pixels that change without motion vectors), `shots.py`, `wind_check.py`, `tree_check.py`, `prepass_check.py`.
- RenderDoc 1.46 is in `.tools\renderdoc`, launched through the `eldenring-recon` me3 profile (user presses F12).
  Captures are in `captures/`, the newest `er117_trees_f2056.rdc` (forest edge). Scripts:
  `qrenderdoc.exe --python tools/rdc_*.py` with `ER_*` environment variables (each script documents them).

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

Goal: make the engine render the scene at e.g. 67% and let DLSS upscale (DLSS input is a sub-rectangle of the
full-size targets, as on the consoles).

What is known (details and addresses in `docs/recon.md`, "Stage 3" sections; addresses are for the current
`eldenring.exe` 2.7.1.0):
- The PC renderer is dynamic-resolution aware: TAA reads `CB_AA` (b0) with `g_vScreenPercentage`, `_Primary`,
  `_Prev`, `_PrevPrimary` at bytes 240-271, and `cbPostProcessCommon` (b4, 64 bytes: `g_dynamicScreenPercentage`
  plus `g_texSizeReciprocal`, the same four pairs). The tonemap and light-shaft passes use
  `ToneMapDynamicResolutionVS`. cbSceneParam has `SC_DynamicResolution_ScreenPercentage` (byte 1376) and a
  secondary one (1392). All are 1.0 on PC.
- The GPU load balancer's degradation list (`0x142F29E50`) has item 19 "Dynamic Resolution" at level 0.
  `LoadBalancerParam` holds console dynamic-resolution settings (`tools/param_patch.py` patches it live), and
  the system property `Performance.IsEnableDynamicResolution` exists. Patching the params and setting the
  properties on 2026-10-08 changed nothing, but how the properties were set is poorly documented, and "level 0"
  may mean "never".
- Dead ends: the render context `[[0x143D61F98]+8]` (+0x9AC percentages; forcing them did nothing) and view+0xF4 in
  `0x141A340F0` (that function builds 0x160-byte per-view blocks, not cbSceneParam).
- Hardware breakpoints (`src/hw_breakpoints.*`) crash the game after about 30 s (anti-tamper). Don't use them.
  Code hooks should be fine (other mods use them).
- The code around the dynamic-resolution property and the load balancer is obfuscated (jump chains), so
  static tracing stalls there.

Static analysis setup:
- Ghidra 12.1.4 + Temurin JDK 21, portable in `.tools\ghidra` and `.tools\jdk`. The analyzed project is in
  `Documents\GhidraProjects\EldenRing` (from `.tools\ghidra_input\eldenring.exe`). Queries:
  `pwsh tools/ghidra/query.ps1 <command> <args>` with `decompile`, `decompileat` (creates a missing function),
  `xrefs`, `calls`, `writes <hexdisp>`, `symbols <text>` (RTTI class names survive), `near`, `ptrs`, `bytepair`.
  Each query takes about a minute (it opens the project headless).
- Quick helpers: `tools/xref.py` (disassemble, RIP-relative references), `tools/funcs.py` (function ranges from
  .pdata, direct callers), `tools/find_stores.py`.

### Next steps, in order

1. **Run the probe** (needs the user in game, any outdoor spot, a few minutes; the game may crash):
   `pwsh tools/ab_capture.ps1 -Round drsprobe`. It sets every float equal to 1.0 in the graphics manager
   (`[0x1447F37A8]`, 0x14000 bytes), two of its sub-objects (+0x13A60, +0x13A68) and the render context to 0.67, a
   few frames each, and logs what changes in `CB_AA`, `cbPostProcessCommon`, cbSceneParam's percentages and the
   scene viewport: `shots/drsprobe_<name>.txt` in the dev folder (the first line is the baseline). Code:
   `src/drs_probe.*`, hooks in `src/addon.cpp` (`ObserveTaaResolution`, the scene observation in `OnDrawIndexed`).
2. If a field moves the percentages: force it (try 0.67), check the scene viewport shrinks and the image is right,
   then feed DLSS the sub-rectangle and output full size (`src/dlss.*`; the TAA replacement is `ReplaceTaa` in
   `src/addon.cpp`). Watch that the post-TAA passes then see 100%.
3. If nothing moves: widen the probe to all private writable memory (look for persistent runs of the four
   percentage pairs), then try driving the GX load balancer (non-zero dynamic-resolution level, or its current
   load level), and as a last resort control render-target sizes and viewports from ReShade.

## Recent commits

```
19faa1b Stage 3 research: resolution-state probe, Ghidra queries, corrected notes
2fb1881 Note that the golden WorldTree_Leaves trees barely move
6e672fa Tree motion vectors: overlay checkbox, lazy start, indirect resolve   <- everyday install
7ee306c Tree motion vectors: SpeedTree through the stream-output path
4a8fbe4 SpeedTree wind dump: last frame's tree clocks can be rebuilt
59deba5 Grass prepass replacement experiment (off by default)
```
