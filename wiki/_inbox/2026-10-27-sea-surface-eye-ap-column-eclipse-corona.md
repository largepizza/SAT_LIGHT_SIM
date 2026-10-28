---
pages: [rendering/sea.md, rendering/atmosphere-and-sky.md, rendering/moon-and-eclipses.md, rendering/refraction.md]
code: [shaders/sat_sky.frag, shaders/include/sky_ap.glsl, shaders/include/eclipse.glsl, shaders/sky_ap.comp]
---
## Now
- Sea glitter at the surface: the sparkle lattice's cells follow the pixel footprint below 0.25 m (down to 1/32 of it),
  holding the 150 facets' density (150 (C / 0.25 m)^2). A cell's flash disc is capped where the slope pdf is near-constant
  over it (r <= sigma, r <= s2 / (2 |m|)); the energy the cap removes stays in the smooth glint (wL = flash / q + 1 - pL / q),
  so the mean is exact at every mirror slope.
- The waves' eye: for the wave trace, normal and glints the eye stands at least 1 m above 0.8 of the highest crest the
  wave octaves can make for the sea state at the eye (static in time); the sky, the air and the depth keep the true eye.
  The trace's bracket never starts behind the eye. Not in Planetarium (SKY_LITE).
- The aerial-perspective volume's per-unit-of-air normalisation (`skyApNorm`) integrates a short segment's column directly
  (Simpson on exp(-h / H) while the segment climbs or falls under half a scale height); longer ones use the Chapman column.
- Solar corona: only near totality (1 - smoothstep(0, 0.02, the share of the Sun seen))^2, and drawn about the Sun as
  seen (the refracted disc's ray). The eclipse's per-step factor in the sky loop / AP volume / sea reflection is one
  function (`eclipseLightK`), exactly 1 outside the penumbra.

## History
- Issue 20261027_212920 (0 m, stormy sea in rain, turning fast): rows of white pixels over the sea were the AP volume's
  normalisation, not the sparkles: `atmColumn` (a difference of two Chapman columns ~35 scale heights each) gave noise of
  ~1e-3 columns for a segment of metres, often 0 (floored 1e-7), and the air in front of rain metres away read
  sums / 1e-7 x noise. White isolated pixels per frame along the report's path 4800-10704 -> 0.
- The user then asked for the eye-at-the-surface case: the 2-m eye was inside a wave field standing 0 .. ~5 m x sea
  state above the sea-level sphere (crest walls through the camera, trace bracket behind the eye), the 0.25-m cell floor
  drew each flash as a white quad tens of pixels wide, and in the slope tail the flash disc outgrew the distribution
  (nearly every cell lit at 1 / q: up to 50x the mean). Normal altitudes unchanged within run-to-run noise.
- Issue 20261027_212202 (partial eclipse at the limb from 160 km, FOV 2 deg): the corona was drawn at (1 - seen)^6 (8% with
  a third of the Sun left) around the UNREFRACTED Sun: a phantom ring under the refracted crescent. The eclipse terms were
  evaluated twice per loop step: GPU 63.5-63.9 -> 57.1 ms there (51.7-52.1 ms the same view without an eclipse).
