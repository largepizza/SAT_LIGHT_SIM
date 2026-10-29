---
pages: [using/features.md, rendering/atmosphere-and-sky.md, development/profiling.md]
code: [src/simulations/Meteors.cpp, src/simulations/SatelliteSimMeteors.cpp, shaders/sat_sky.frag]
---
## Now
- Meteors: past 2x of time warp only fireballs and bolides (absolute mag <= -4) are drawn, up to 1 min/s; faint
  meteors only up to 2x, none past 1 min/s. Which meteors exist never depends on the warp or the eye: the bin cache
  serves every eye within 30 km of where it was made and generation widens its reach by that margin. Persistent trains
  decay at `meteors.train_duration` (0.3, no UI; 1 = the earlier, longer trains) times the physical decay time and are
  cut at 4 decay times (a -12 bolide's train: ~65 s, was ~270 s).
- The Sun's glare and corona are veiling (eye / lens) light added over everything in front: clouds, the Earth,
  terrain, the sea. They follow the light reaching the eye from the Sun: the cloud composite and the scene depth sampled
  at 5 points over the Sun's drawn disc on screen, the eye's Sun-line cloud transmittance, and the disc's visible share.
- The aerial-perspective volume's horizon band (where the per-pixel loop still runs) is 2 deg but at most 12 screen
  tiles, so at fields under ~30 deg the band narrows with the tiles.

## History
- Issue 20261028_191529 "Bollide Trails": the streak was a -12.2 N Taurid's train 36 s after the bolide; it reproduced
  after rewinds, time jumps and eye moves, but not when replayed past 2x (meteors were then off entirely), and a climb
  under one cache (2 -> 11 km) shortened the reach by ~200 km.
- Issue 20261028_190911: from 1625 km the halo over the night side was multiplied by each pixel's own (far-layer) cloud
  transmittance: every dark cloud cut a black hole in the glow.
- Issue 20261028_192855 (0.58-deg field on the refracted Sun from 96 km, RTX 3070 Ti 1920x1009): the fixed 2-deg band
  covered the whole screen; sky pass 34.6 -> 9.3 ms (FOV 5: 23.8 -> 10.9; FOV 40 unchanged). Against a 64-4096-sample
  loop the volume is within ~1 level; the default loop had read the ground under the limb ~15 levels dark. The clouds
  there (16-24 ms) are content (same at a 40-deg field); an LOD floor at narrow fields saved ~2 ms and was not adopted.
