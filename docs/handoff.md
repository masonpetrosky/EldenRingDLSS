# Handoff: Elden Ring DLSS, state on 2026-10-12

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
| DLSS Ray Reconstruction | Everyday and dev installs (2026-10-11); with our own ray generation shaders (below) |
| Own ray generation shaders (render-size AO, 1 ray, blue noise) | Everyday and dev installs (`RtShadowRayGen=3`); 0.2.0 defaults to 4 (temporal) |
| 4. Release | v0.1.0 on GitHub (release zip) and Nexus Mods (2026-10-11) |
| Motion vectors for enemies and animals | Everyday and dev installs (`NpcMotion=1`), +0.2-0.3 ms (2026-10-11) |
| Tree motion vectors at 50% | Were lost from 2026-10-10 (resolve viewport); fixed, everyday install updated |
| Object-only motion blur | Everyday and dev installs (`ObjectBlur=1`), in daily use with the game's blur on (2026-10-12) |
| NVIDIA Reflex (injected at the present) | Everyday and dev installs: GPU queue 8 ms shorter, user feels it; ~3% fps |
| Fast travel left the game at full size (grain under RR) | Fixed (6c124ae), everyday and dev installs (2026-10-12) |
| GPU profile (5 places) | Done (2026-10-12): `docs/raytracing.md`, "GPU profile: where the frame goes" |
| Foliage cost in our rays; leaf-shadow popping; a flicker | **Open**, dev install only (83545fe, visual validation pending): see Next steps 0 |
| 0.2.0 release (fast-travel fix, preset E, RR checkbox, water reflection position) | GitHub + Nexus, everyday install (2026-10-19) |
| Native-size water reflections (`WaterReflections=1`) | Works in game (2026-10-20, test copy `dist/test-0.3.0`); `docs/recon.md` |
| Water flickered between the native and the engine's reflection | Fixed (hooks now run in lists decided at submission); user: "water is great now" |
| Game update 1.17.2 moved the TAA filter's vtable | Fixed (RTTI lookup); 0.2.0 needs it: release pending |

The user chose NVIDIA's placement for the upscale (DLSS at the TAA, before tonemapping; "option 1") over upscaling
the tonemapped image at the final composite.

## Installs

Source changes after the 2026-10-12 audit were deployed to **dev only** at 12:52 local time and the game launched
with `eldenring-dlss-dev`; the everyday install is unchanged. The deployed add-on's SHA-256 is
`1307DF1FCA83C6C143BBD58F1B21EA23AE67206A0FCC72224C3BA39C31804D7F`. Its previous add-on and configuration are
backed up in `build/audit/dev-before-fixes-20261012-125204`. After the user's "ready", remote disable/re-enable,
50% -> 67% -> 50%, RR -> SR -> RR, and 50% -> DLAA -> 50% passed in game. NPC patches matched prior frames
with no refused windows; they measured 0.30-0.33 ms in two 60-frame profiles at this spot (no controlled old-build
comparison). Object-blur off/on at DLAA stopped and restarted its transforms. The user pressed physical F9:
Enabled=0, native rendering, our AO off, and its counters fixed at 20339 across two queries; remote `dlss on`
restored 50% and our AO. Output resolution changed to 2560x1440: RR recreated at 1280x720, AO/blur resumed,
the game remained responsive, and evaluation failures stayed zero. Returning to 3840x2160 also recreated RR at
1920x1080, resumed AO/blur, and kept evaluation failures at zero. After the user quit normally, the dev profile
was launched again at 13:06 with Enabled=1, RenderScale=1, ObjectBlur=0 for the startup check. After the user's
"ready", blur stayed at zero camera copies/transforms while off. `objblur on` initialized it live: 60 copies,
59 transforms after two seconds, then 226/225. The original baseline was restored and its ini matches the
pre-test ini exactly: DLSS/RR on, scale 0.50, object blur on, NPC motion and Reflex on. The user subsequently
quit normally; the game and me3 launcher are confirmed closed. The pre-startup ini is backed up at
`build/audit/in-game/before-native-startup.ini` (Enabled=0 persisted by the physical F9 test, though remote
`dlss on` restored the first session's runtime); the original baseline ini with Enabled=1 is in
`build/audit/dev-before-fixes-20261012-125204`. First-session runtime log:
`build/audit/in-game/ReShade-first-session.log`; startup-check log: `build/audit/in-game/ReShade-native-session.log`.
Computer Use screenshot capture worked, but injected F9 and Escape did not register.
Evidence is `build/audit/in-game-validation.txt` and `build/audit/in-game/`. RR processing
now follows the enabled settings and successful evaluations, destroyed textures leave the caches, scale increases
create enough input capacity, NPC bone windows are private to each command-list recording, object blur starts state
tracking when enabled live, and the SDK fetch downloads its complete header set without GitHub CLI. Standalone
regressions are in `tests/regressions.cpp` (`pwsh build.ps1 -Tests`; optional native checks:
`build/erdlss_regressions.exe --gpu third_party/dlss/bin/rel`). The targeted F9, scale, resolution-recreation, and
native-startup blur checks passed. Broader visual checks of NPC motion in crowds and a controlled old/new NPC
performance comparison remain. An unexpected NGX error can still affect the
frame whose lighting was already recorded; later frames restore the game's denoisers and recreate the feature.

Two me3 profiles under `%LOCALAPPDATA%\garyttierney\me3\config\profiles`:

- **Everyday** (`eldenring-default`, Start menu "ELDEN RING (me3)"): UnlockTheFps plus
  `natives\EldenRingDLSS` (release DLSS DLL). Build of commit 6c124ae, installed 2026-10-12 morning with the user's
  OK: d0f03f5 (installed after midnight) plus the fast-travel fix and the dev-only GPU profiler. 50% with preset M, enemy and animal motion vectors (`NpcMotion=1`) and the trees' motion at 50% fixed,
  object-only motion blur (`ObjectBlur=1`) with the game's Motion Blur on: the user tried it in this install and
  keeps it ("It seems great"), NVIDIA Reflex injected at the present (`Reflex=1`; the user noticed an improvement),
  on top of the jitter, distortion/sparks/grace-flame fixes and distance-based ray tracing refits. Ray
  Reconstruction on with our ray generation shaders (`RayReconstruction=1`, `RtShadowRayGen=3`, release
  `nvngx_dlssd.dll` 310.9.1), the dev install's setup, turned on the same night at the user's request: measured +1.9
  ms over DLSS M in its first version, about 1.6 ms of that recovered since (render-size AO, 1 sun shadow ray).
  Its ini: `Preset=13`, `RenderScale=0.50`, game sharpening off, game exposure on, chromatic aberration off,
  `WindMotion=1`, `WindMode=1`, `WindTrees=1`, `NpcMotion=1`, `RayReconstruction=1`, `RtShadowRayGen=3`,
  `ObjectBlur=1`, `Reflex=1`. Backups in `build\everyday-backup\`: d0f03f5 (the previous everyday build, before
  the fast-travel fix), bba1275 (before Reflex), ff56979 (with RR), ff56979-noRR
  (its ini before RR), 91d6b47 (the previous everyday build, 50%, M),
  b2ac485 (58%, K), 7182d94 (67%, K), f0e8c12 (DLAA). Only update it with the user's
  explicit OK and with the game closed (`tools/deploy.ps1 -Target main`).
- **Dev** (`eldenring-dlss-dev`, `natives\EldenRingDLSS-dev`, developer DLSS DLLs including `nvngx_dlssd.dll`):
  test builds only. Latest build: the post-audit fixes (2026-10-12, **targeted regression checks passed** above),
  including the six source fixes above and 83545fe's l_rayGenCB edit fix and refits
  following their animation, on top of 6c124ae, the everyday build. Since 56dbb23: Ray Reconstruction (`RayReconstruction=1`,
  used only while the game's ray tracing runs) fed by our own sun shadow ray generation shader with render-size AO
  (`RtShadowRayGen=3`: blue noise, 1 AO and 1 shadow ray per pixel). Ini: `RenderScale=0.50`, `Preset=13` (M),
  `GameExposure=1`. F9 toggles DLSS and saves `Enabled` to the ini: check `Enabled=1` (and the "Loaded. DLSS on" log
  line) before measuring.
- A test copy of the release zip lives in `dist/test` (profile `dist/test/EldenRingDLSS.me3`, and
  `EldenRingDLSS-fps.me3` with UnlockTheFps for frame-rate measurements); not an install the user plays with.

The user's game settings: Custom, almost everything Maximum (water, shader and GI quality High), ray tracing
Maximum, motion blur off, 4K, HDR on. Game version: App Ver. 1.17.2 = `eldenring.exe` 2.7.2.0 since 2026-10-20 (1.17.1
= 2.7.1.0 before; a copy of that exe is in `Documents\Tools\Ghidra\input`). After a game update, run
`build/erdlss_regressions.exe --exe <eldenring.exe>` before testing in game.

## Working rules (from the user)

- Ask the user to be at the PC before launching the game, and wait for "ready" (loaded in, controls released)
  before in-game tests. Never load into the world without them (their character could be attacked). For daylight,
  they can rest at a grace and "Pass time" to Noon.
- Questions requiring the user's action stay pending until they answer. Do not put a response deadline or a
  countdown on these questions; the user explicitly requested this on 2026-10-12.
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
- NPC motion (dev remote): `npc` (stats; `npc on|off` switches and saves), `npcstage 1|2|3`, `npctest <m>` (shifts
  every previous pose so everything the path draws shows motion), `npcdist`, `npcbones`. `Dred=1` in the ini turns on
  D3D12 crash breadcrumbs; after a GPU hang the report is in the log and `shots/dred.txt`.
- `tools/mem.py` reads, searches and writes the running game's memory from outside; works under RenderDoc too.
- Analysis scripts in `tools/` (Python venv at `.tools\venv`): `mv_check.py` (motion dumps), `shots.py`,
  `wind_check.py`, `tree_check.py`, `prepass_check.py`, `blue_noise.py` (regenerates `src/blue_noise.hlsli`).
- RenderDoc 1.46 is in the shared `Documents\Tools\RenderDoc`, launched through the `eldenring-recon` me3 profile (user presses F12;
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
- For the next release: the comparison slider pairs the game's TAA (sharpened) with unsharpened DLSS, captured 36 s
  apart while the light changed; a fairer pair would be back-to-back, possibly with `GameSharpening=1`.
- **0.2.0, prepared 2026-10-19, not released.** The Nexus page had 323 unique downloads and six unanswered comments,
  two of them about no frame-rate gain: 0.1.0 stops upscaling after the first loading screen (fixed in 6c124ae).
  Zip in `dist\` (`CHANGELOG.md`) with NpcMotion=1 in its ini, regression and GPU checks passed. Rebuilt the same day
  after the user asked for Ray Reconstruction to be featured: an overlay checkbox with a state line, `RtShadowRayGen`
  3 by default (modes 2-4 now replace the game's rays only while RR runs; otherwise the game's shaders run, not our
  port), and `ab_capture.ps1 -Round rrshow` for the page's RR images (SR, RR with our rays, RR with the game's rays,
  TAA; each with frame times and the DLSS ones with a GPU profile).
  The remote control works in release builds too, so `dist\test-0.2.0` (the zip plus ReShade64.asi, RenderScale=0.50,
  `EldenRingDLSS-fps.me3` with UnlockTheFps) can run `ab_capture.ps1 -Folder <its EldenRingDLSS folder>` rounds.
  Waiting for one in-game pass by the user (log, showcase screenshots and frame rates, fast travel, NPCs, leaf
  shadows under trees for 83545fe, presets K/M/L/E, Ray Reconstruction from the new checkbox), then the release, the
  Nexus file, page and replies (drafts in
  `dist\nexus\page-0.2.0.bbcode` and `replies-2026-10-19.json`, local only).

## Ray tracing with our own ray generation shaders (dev)

Details: `docs/raytracing.md`, "Our own shadow ray generation shader in the game's pipeline".
- The add-on hooks the native device's `CreateStateObject` (vtable slot 62) below ReShade, because ReShade 6.8
  rebuilds edited state objects without their local root signatures, and appends `rt_raygen.hlsl`'s library plus an
  association to the game's ray generation local root signature. The sun shadow dispatch runs ours through a copy of
  its ray generation record in the add-on's upload ring; the game's shader table is never written.
- With RR (modes 2-4): 1 AO ray per pixel at the render size into the shadow output's unused 4th channel, copied
  after the dispatch and drawn into the game's AO target (`ao_upscale.hlsl` PSFull); the game's half-size AO dispatch
  is skipped; 1 sun shadow ray per pixel instead of 2. Cheaper than the game's path (about 1.6 ms at 4K/50%).
- The game's sin hash made a "mirage" wave in distant grass under RR; PCG or blue noise fixed it. Temporal blue
  noise (mode 4, a frame index written into `g_sceneCB`'s padding) is the default since 2026-10-19, the user's pick
  ("it looks great"); plain blue noise (mode 3) looked the same in a live A/B.
- Noise the user still sees in grass at 50% is the upscale, not the ray tracing (it stays with the game's AO, with
  the shadow denoiser on and with RR off; calmer at 67% and DLAA). The user keeps 50% for the frame rate (50% 58,
  58% 49, 67% 46 fps at that spot with RR).

## Next steps

0. **Resume here: broader NPC motion checks, then test the 83545fe foliage changes** (game closed;
   targeted post-audit checks passed; original 50% DLSS/RR settings restored;
   `docs/raytracing.md`, "Foliage in our
   rays, and a flicker"). At the second forest spot (the user's character beside leaves that popped):
   a. After loading in, remote `rtsamples`: more than one distinct set (different shadow fields) confirms the cause.
   b. Screenshot bursts a minute apart (`capture fl_<name><i>` x8; center brightness steady across the 8) and the
      user's eye: no whole-view flicker, no popping leaf shadows. `rtinfo`: refits following their animation
      against the schedule (about 26 per frame there).
   c. If stable: `ab_capture.ps1 -Round rtfoliage` again, then the AO alpha A/B (`rtalpha off on` vs `rtalpha on on`,
      4 s each; the user looks under the trees by brightness). If AO without the alpha test stays invisible, make it
      the default while our AO feeds RR (was in the flickering build, taken out until re-checked).
   d. Then the everyday install, with the user's OK: its 6c124ae writes back only the ray counts and seems free of
      the flicker, but has the refit/animation disagreement (leaf shadows that pop near the player).
1. **Fast-travel fix in everyday play** (6c124ae, in the everyday install since 2026-10-12): before it, after any
   loading screen the engine stayed at full size, DLSS and RR stopped, and the game's denoiser got our 1-ray AO and
   shadows (grain on the character, walls and ground). Tested in the dev install (two fast travels, back at 50% within
   about 150 ms), not yet in the everyday one. If grain shows after a load, the log's last `Scene: screen percentage`
   line tells whether the scale came back (0.500); in the dev install remote `preset 13` recovers it.
2. **Savings from the GPU profile** (`docs/raytracing.md`, "GPU profile: where the frame goes"): no 3-5 ms of
   redundant work; RR is a fixed 4.1 ms. In order: our rays in the forest (1.1 -> 4.5 ms; likely alpha-tested
   leaves: shorter AO rays or opaque foliage for AO), acceleration-structure builds in fights (up to 2.9 ms), the
   game's shadow denoiser still running under RR (0.17 ms), `SharpenFilter` with sharpening off (0.07 ms), the
   stretch (0.5 ms). Profile again with `gpuprofile 60 <place>` and `gpu_profile.py --compare`.
3. **Reflex** (docs/recon.md, "NVIDIA Reflex, injected at the present"): the sleep works (8-9 ms per frame, the frame's
   GPU work queued 8 ms less) and the user noticed an improvement. Open: whether the game reads input after the sleep
   (hook its input call and timestamp it against the markers), and a hook at the game's own frame start if the input
   timing shows the sleep is too late. In the everyday install since 2026-10-12.
4. Object-only blur, open edges: idle with the game's Motion Blur off is untested by measurement (bba1275 gates it on
   the blur pass; with the blur on the user tested this build in the everyday install). Watch for a pixel of blur on
   silhouettes while strafing and on swaying grass and trees (docs/recon.md, "Object-only motion blur").
5. RR's responsivity mask on vegetation (preset F only; negative values favor stability) for the grass at 50%.
6. The outreach drafts above, one at a time with the user.
7. NPC motion edge: an enemy part shared with the player's equipment gets no motion (excluded with the player's
   parts by mesh key). Check first whether it happens: `npc` reports draws left to the game per frame, which should
   equal the player's part count; fighting soldiers in the player's armor would show more.
8. Older items: broader robustness (the first frame of a new command-list kind; targeted live scale and resolution
   transitions passed on 2026-10-12), performance of the stretch and full-size post-processing (about 1 ms), and
   removing dead experiments (`drs_probe.*`, `engine_resolution.*`, `hw_breakpoints.*`).

## Recent commits

See `git log`. 2026-10-12: GPU profiler (fe36062, resolve fixed in 7f1dee1), fast-travel fix
(6c124ae), foliage switches and the refit/flicker fixes (83545fe, untested). 2026-10-11: release packaging and README (bf33a45, d20a54c, 28007e3, 3962d3e); our ray generation
shaders in the game's pipeline (6d7c0b1), render-size AO into RR and PCG noise (9c60c29), blue noise (23b8223),
temporal blue noise option (9ffc216), 1 sun shadow ray (56dbb23), the NPC motion vector finding (7883869).
