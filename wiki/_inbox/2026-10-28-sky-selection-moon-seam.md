---
pages: [rendering/moon-and-eclipses.md, rendering/atmosphere-and-sky.md, using/controls.md, using/graphics-settings.md]
code: [shaders/sat_sky.frag, shaders/sky_taa.comp, src/simulations/SatelliteSimMoonBases.cpp, src/simulations/SatelliteSim.cpp, src/simulations/SatelliteSimUI.cpp]
---
## Now
- The Moon's disc (full sky variants) is anti-aliased: each colour channel's coverage is the angle off the disc's
  centre, measured as the small offset vector, over one input pixel; the normal comes from the lateral offset. Moon
  pixels carry the sky TAA marker 0.975, and with the view still the resolve never takes a Moon pixel's raw sample
  (no "flash rule" there), so the disc accumulates. Still view at 1538 km, FOV 0.54 deg: pixels varying > 4 levels
  1.0% -> 0.08%, > 8 levels 0.48% -> 0.006%.
- The Milky Way's main fetch uses textureGrad with the longitude wrap taken out of the derivatives: no seam at the
  galactic anticentre (u 0/1, l = 180).
- The zoomed star clouds are tunable (Photometry > MILKY WAY STAR CLOUDS (ZOOMED IN), keys
  `photometry.milky_way_star_zoom_from` 0.6 / `_zoom_full` 0.3 (input px per panorama texel), `_fill` 0.9, `_size_px`
  0.9, `_spacing_px` 4, `_brightness_spread` 0.65, `_faint_from` 0.003 / `_faint_full` 0.02, `_nebula_exclusion` 1).
  The Milky Way look rows (gain ground / space, contrast, night vision, zoom detail) are now shown in a MILKY WAY group.
- A Moon-base glint is selectable (click within 8 px of a drawn glint): a panel with the base, magnitude seen / above
  the air, arrays (area, rows, built share), the Moon's phase angle, the glint window, rows lit; Track follows it.
- Picking: a Galilean moon is pickable only when clear of Jupiter on screen (> 8 px from its centre or 4 px outside its
  disc), so a click on Jupiter at a wide field picks Jupiter. The pick chain (comet, Moon-base glint, planet, star,
  satellite) is one function, also harness `select pick <x> <y>`.

## History
- The Moon's "flicker" from orbit (issue 20261028_191038): with the sky TAA off the disc was frame-identical; the TAA's
  flash rule reset crater rims and the limb to each jittered sample, and the limb's hit test (bm^2 - (1 - R^2))
  cancelled in float to ~1 px of the radius: a jagged staircase. Refraction was not involved (no change with it off).
- The thin dark line across the sky (issues 20261028_190641 / _191529) was the implicit mip level at the panorama's
  wrap (derivative ~1 -> the coarsest mip), present since the panorama was added.
