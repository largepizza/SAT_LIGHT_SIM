---
pages: [rendering/cities.md, rendering/sea.md, rendering/atmosphere-and-sky.md]
code: [shaders/sat_point.vert, shaders/sat_point.frag, shaders/sky_taa.comp, shaders/sat_sky.frag, src/simulations/SatelliteSim.cpp]
---
## Now
- City light sprites are depth-tested in the fragment shader against the unjittered half-res scene depth (at
  0.95 of their range), not against the hardware depth the sky TAA restores (this frame's jittered sample).
- The sky TAA resolve, when nothing moved this frame, also accepts this frame's 3x3 depth range in its
  disocclusion test (still frames only).
- "Still" and "frozen" for the sky TAA come from the sim time itself: frozen = sim time unchanged since the last
  resolve, still = it moved at most 1.5x the frame step. A cinematic (which holds the pause flag while it sets sim
  time) no longer counts as frozen.
- The sea reflection's overcast factor reads the filtered cloud shadow (the [1 2 1] tent), never the raw tap.

## History
2026-10-06, user reports before the 1.2.0 release:
- "Z-fighting" of far city lights at low angles: on the r35 Irvine horizon the worst pixels alternated ~240 / ~37
  levels every frame — sprites just behind a crest, hidden or drawn by turns with the jitter. Still-view flicker
  0.57% -> 0.31% of pixels (worst pixel std 103 -> 34); climb benchmarks unchanged.
- "White static" on the ocean: a fixed grid of period 2 half-res texels (the cloud shadow's 2x2 ordered march
  offsets) in the sea reflection, shimmering as the waves moved. Period-4-px energy 4.4x -> 1.0x its neighbours'.
- Cinematic playback over the sea: the waves moved under an unclipped history (grey crest trails); pixels varying
  > 8 levels 15.6% -> 4.5%.
