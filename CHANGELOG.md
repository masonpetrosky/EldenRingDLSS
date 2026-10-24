# Changelog

## Unreleased

Fixes:
- Particle effects under upscaling (render scales below 100%): a new effect's first frames, and an effect drawn on
  the other side of the game's TAA than before, were drawn at the wrong size and place for a frame or two, so
  particles seemed to move the wrong way and snap back (reported on Nexus with the embers after the Erdtree burns).
  The add-on now decides when the effect is sent to the GPU instead of guessing from earlier frames.
- The radial blur when a Living Jar breaks showed a box of a blurred, wrong part of the screen below 100%. Every
  particle effect that reads a copy of the screen now gets its rectangle corrected, not only the heat distortion
  (which also fixes a full-screen distortion the old name check missed), and every particle effect's screen-size
  constants are corrected (lit particles used the render size for their light tiles).

## 0.4.0 (2026-10-21)

New:
- DLSS Frame Generation (`FrameGeneration`, experimental, off; also in the overlay): DLSS-G through NVIDIA Streamline
  shows generated frames between the game's own, made from the game's depth and camera, the add-on's motion vectors
  and the frame without its HUD, so the HUD stays sharp. 2x on RTX 40 and 50 cards, 3x to 6x or dynamic on RTX 50;
  Reflex runs with it. Needs hardware-accelerated GPU scheduling, and a restart to switch on from off. At a castle
  courtyard in 4K at 50% with Ray Reconstruction on an RTX 5070 Ti: 70 fps off, 120 at 2x, 168 at 3x, 208 at 4x,
  evenly paced. Not available under Wine or Proton.

Fixes:
- Places with many enemies or NPCs in view ran slowly with `NpcMotion=1`: each character's previous pose waited for
  the one before, so 300 of them at a castle courtyard took 5.3 ms per frame. They now run together in 0.2 ms; that
  courtyard went from 52 to 70 fps (4K at 50%, Ray Reconstruction, RTX 5070 Ti).
- Tree motion vectors (`WindTrees=1`) cost about 2 ms in forests: each tree draw's wind patch made the GPU wait in
  the middle of the game's G-buffer pass. The patches now run together before it, and the trees cost about 0.5 ms:
  in a Limgrave forest 48.8 fps instead of 45.6, against 49.7 with the trees off (4K at 50%, Ray Reconstruction,
  RTX 5070 Ti).
- Under Proton the game could stay on a black screen at start-up (GitHub issue #1): the add-on started DLSS inside
  the game's device creation, and under Proton that start never returned. DLSS and Reflex now start on a thread of
  their own once the device exists, and the game keeps its TAA until DLSS is ready. Not yet confirmed on Linux.

Changed:
- Tree motion vectors are on by default again (`WindTrees=1`, a checkbox in the overlay), now that they cost about
  0.5 ms in a forest at 4K and 50%. An `EldenRingDLSS.ini` kept from 0.2.0 or 0.3.0 still has `WindTrees=0`.

## 0.3.0 (2026-10-20)

Fixes:
- ELDEN RING App Ver. 1.17.2 (`eldenring.exe` 2.7.2.0): the update moved game code the add-on looks for, so 0.2.0 no
  longer found the TAA filter while upscaling. It upscaled with the game's 8 jitter positions (static detail turns
  into a fine dot pattern) and with the scene's jitter, which is now and then a frame stale (presets M and L make the
  world jump). The add-on now finds the filter by its class name in the game's type information instead of a fixed
  address.
- Ghosts and other characters drawn after the lighting (see-through and glowing ones) had no motion vectors, so DLSS
  smeared them as they moved.
- NPCs wearing the same armor or body mesh as your character had no motion vectors on those parts.

New:
- Water reflections at the native size while upscaling (`WaterReflections=1`, on by default, also in the overlay):
  the game traces the water's screen-space reflections at half the render size, so at 50% they had a quarter of the
  native detail and sparkled. The add-on now traces them at the size the game uses at 100%: about 0.2 ms and 170 MB
  of video memory at 4K and 50%.

## 0.2.0 (2026-10-19)

Fixes:
- Upscaling only lasted until the first loading screen: after fast travel, death or any other loading screen, 0.1.0
  fell back to native resolution with the game's TAA until the game was restarted.
- Water reflected the wrong part of the screen while upscaling (render scale below 1): the reflections came from the
  top-left of the view, so a fire or the stars could shine in the water far from where they are.
- Tree motion vectors were missing while upscaling (render scale below 1).
- With ray tracing on, leaf shadows near the player could pop: the add-on's refit schedule now follows each group's
  animation.
- Switching DLSS and Ray Reconstruction on and off, changing the render scale and changing the output resolution in
  game are more robust.
- F9 no longer switches DLSS while Alt, Ctrl or Shift is held (Alt+F9 is the NVIDIA App's record shortcut).

New:
- Motion vectors for enemies, NPCs and animals (`NpcMotion`, on in the zip's settings, about 0.2-0.3 ms). The game
  writes motion vectors for your own character but not for most other characters, so DLSS smeared them.
- Preset E, the older CNN model (`Preset=5`, also in the overlay): about half of K's cost. DLAA at 1440p on an RTX
  5070 Ti: E 0.36 ms, K 0.83 ms, M 2.62 ms. The overlay's "Default" entry now names the preset NVIDIA picks for the
  mode (M at Performance, not K).
- Object-only motion blur (`ObjectBlur`, off): with the game's Motion Blur on, only moving things blur, not camera
  turns.
- NVIDIA Reflex low-latency mode (`Reflex`, experimental, off).
- Ray Reconstruction in the overlay: a checkbox, with a line that says whether it runs or what it waits for (DLSS,
  the game's ray tracing).
- Under Ray Reconstruction the add-on now traces the ambient occlusion and sun shadows itself (`RtShadowRayGen=4`, the
  new default): one ray each per pixel at the render size with temporal blue noise, where the game traces AO at a
  quarter of the pixels. Sharper contact shadows in grass, no wave pattern in distant grass (the game's random
  numbers made one that its denoisers had hidden), and cheaper. Without Ray Reconstruction the game's own rays run,
  as before.

Changed:
- Tree motion vectors are off by default (`WindTrees=0`, a checkbox in the overlay): they cost about 2 ms at 4K with
  trees in view. Turn them on if swaying canopies smear. Grass and bushes keep theirs.

## 0.1.0 (2026-10-11)

First release: DLSS in place of the game's TAA (DLAA, or upscaling through the engine's dynamic resolution), motion
vectors for swaying grass and trees, cheaper ray tracing, experimental Ray Reconstruction (`RayReconstruction=1`).
