# Changelog

## 0.2.0

Fixes:
- Upscaling only lasted until the first loading screen: after fast travel, death or any other loading screen, 0.1.0
  fell back to native resolution with the game's TAA until the game was restarted.
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
- Ray Reconstruction (experimental): the add-on's own ambient occlusion and sun shadow rays (`RtShadowRayGen=3`),
  cleaner and cheaper than the game's rays under Ray Reconstruction.

## 0.1.0 (2026-10-11)

First release: DLSS in place of the game's TAA (DLAA, or upscaling through the engine's dynamic resolution), motion
vectors for swaying grass and trees, cheaper ray tracing, experimental Ray Reconstruction.
