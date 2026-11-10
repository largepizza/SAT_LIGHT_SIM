---
pages: [rendering/terrain.md, rendering/hardware-tiers.md, using/graphics-settings.md, development/profiling.md]
code: [shaders/include/terrain_clip.glsl, shaders/terrain_clip_bake.comp, shaders/include/terrain_detail.glsl, shaders/scene_depth.comp, src/simulations/SatelliteSimTerrainClip.cpp, CMakeLists.txt]
---
## Now
The terrain detail's value octaves are read from a height clipmap: 11 nested 1024x1024 levels round the observer (level j
= the octaves at the geometry LOD 1 m x 2^j, texel lod / 3, toroidal, on the tangent plane at an anchor point), baked by
`terrain_clip_bake.comp` as the observer moves (only the strips that enter a level's window; everything on a jump or
when a terrain slider or the launch pads change). The three terrain marches of a frame (the scene-depth pass at quarter
and half resolution, the sky pass at full resolution) read two levels and blend them with the octave's own LOD weight,
which reproduces the procedural height exactly up to the bilinear texels. The erosion octaves are not baked (their
gullies crease at pixel scale): they are evaluated once per march and taken as a plane round that sample, as before.
The shading normal's octaves stay procedural. Setting `clouds.terrain_clipmap` 1 (0 = the procedural shaders), no UI.
113 MB of VRAM (46 MB heights r32f, 67 MB rgba16f: the terrain shadow's coarse surface + the coarse gradient). Needs 18
samplers in the sky's fragment stage: a device at the 16-sampler floor runs the procedural shaders.
Performance (RTX 3070 Ti, 1920x1009, the user's settings, GPU frame): ground views -5 to -7.5 ms (clear desert 21.3 ->
16.3, overcast 27.1 -> 21.4, terminator 27.0 -> 21.8, the Reflect-beam site 29.2 -> 23.5, the Alps from the ground 28.8 ->
21.4, the Grand Canyon rim 33.9 -> 27.3); from 6 km up unchanged. The procedural detail was 7-10 ms of a ground frame.
Images within the recapture floor at every view but the nearest ground (0.66 levels against 0.35 at the Alps from 2 m),
which is inside the march's own tolerance (a 20x tighter march is 1.7 levels from both).
The scene-depth pass also computes the observer's ground height once a frame (a one-workgroup frame pass) instead of in
every texel. `state` reports `observer.terrain_clip`.

## History
A first cut baked the erosion too: 10-12 ms saved, but soft gullies and flat low-Sun shading; dropped the same day.
