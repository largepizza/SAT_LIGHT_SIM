---
pages: [rendering/points-bloom-glare.md, rendering/satellite-meshes.md]
code: [shaders/mesh_bloom.frag, src/simulations/SatMeshRenderer.cpp, src/simulations/SatelliteSim.h, src/simulations/SatelliteSimUI.cpp]
---
## Now
A resolved satellite model seeds the bloom by its own displayed brightness: per flare texel, the larger of the
energy-matched sprite seed and the sprite's log response b = clamp(log2(x / 2) / 2, 0, 4) on the pixel's displayed
value x = luminance x sky exposure (texel-averaged). A model clipping to white glows like a sprite of its own size;
pixels at or under twice white add nothing. Setting `photometry.model_glare` (Settings > Photometry > Bloom and
glare, "Model glare", default 1.0, 0 = off).
The mirror's starburst stays the sprite's (glare.vert), sized by the glare size x the proximity gain.

## History
Issue 20261112_195153: a Reflect mirror at 2 km reflecting the Sun (mag -28.7) drew as a white quad on black. The
satellite adaptation (8 mag, raised by the mirror itself) dimmed the sprite whose seed the mesh bloom was
normalised on, and the seed was a sprite's few texels spread over ~3000. The starburst sat inside the 240 px white
mirror only because the reporter's `glare_near_range_km` is 1.0 (default 21.86). Halo to 1% of white: 3 px before, 73 px after.
