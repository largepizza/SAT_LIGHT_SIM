---
pages: [rendering/clouds/march.md, rendering/atmosphere-and-sky.md, development/harness.md]
code: [shaders/lightning.frag, src/simulations/SatelliteSimHarness.cpp, src/simulations/SatelliteSimLightning.cpp]
---
## Now
A lightning flash's light on the clouds is drawn by `lightning.frag` (mode 1) after the sky TAA, from the half-res
cloud composite. Near a ridge each of its 2x2 texels is classed by the scene depth under its centre (ground or sky);
where all four are ground the neighbouring blocks one texel over are tried (the half-res depth stands a texel proud
of the real silhouette). The light is taken from the sky texels alone (bilinear weights renormalised) and drawn just
past the ground those texels hit through `gl_FragDepth`, so the main pass's full-resolution depth (the terrain
march's own per-pixel silhouette) cuts it off the ridge. A ridge against flash-lit cloud now follows the same
1-px silhouette as in daylight; cost +0.08 ms while a flash is up. Harness: `shaders reload bolt=<spv>` rebuilds the
lightning draw from a variant SPV (`bolt=` empty: the shipped one).

## History
2026-11-23 (wave c24, .plans/TERRAIN_EDGES_PLAN.md phase 0 + 1.2): the user saw "a hard, pixelated boundary between
the lit sky and the ground" during flashes. Phase 0 at Chamonix (1920 x 1009): the 2-px staircase stayed with the
TAA on, off and on a flash's first frame, and with the flash light by distance alone; it went with mode 1
discarded. A sunlit ridge against sky or cloud, and a near ridge over a far one, step at 1 px (the march) with the
clouds, the AP volume or the fog knocked out: no coarse layer there.
