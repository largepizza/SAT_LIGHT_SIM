---
pages: [rendering/terrain.md, rendering/atmosphere-and-sky.md, rendering/aurora-airglow.md, development/harness.md]
code: [shaders/include/terrain_detail.glsl, shaders/scene_depth.comp, shaders/sat_sky.frag, shaders/cloud_march.comp, src/simulations/SatelliteSimHarness.cpp]
---
## Now
The procedural terrain detail is skipped where its octave-0 amplitude is under ~4 cm (zero below 2 cm, eased in to
4 cm): low flat land such as deltas and coastal plains. The half-resolution terrain sun shadow (scene depth pass) covers
all land the sky pass shades as terrain, including land the DEM reads at 0 m. Over water the terrain march steps up to
the water map's distance to any land, less 4.5 km and at most 12 km a step (not near a coast patch, a launch pad or a
storm surge). In the horizon band, the per-pixel atmosphere loop stops just past the air in front of an opaque cloud.
The aurora march is skipped for rays sunlit at every point and behind opaque cloud seen from below. Harness:
`shaders reload depth=<spv>` swaps the scene depth pass for an interleaved A/B.

## History
Perf pass 5 (2026-10-29), RTX 3070 Ti, 1920x1009, the user's settings, interleaved A/Bs in one app alone on the GPU:
Shanghai from 3 km -4.1 ms GPU, 1 km -2.6, on the ground -3.0, open sea from 20 m -1.8, storm from the sea -1.6, storm
views at 12 km -0.5..-0.9. Matrix suite (`--quick`, still GPU summed over 21 cells) 472.0 -> 444.5 ms, 0 regressions.
Before, centimetre-amplitude octaves ran on every low flat pixel, 0-m land fell to a per-pixel shadow march, and a grazing
ray over the sea took ~200 small steps to reach the horizon. Tried and dropped: keeping the cloud clipmap on while moving
(-6 ms on the user's storm trail, but cloud masses vanish: the bake is footprint-dependent; clearing stamps per epoch did
not help).
