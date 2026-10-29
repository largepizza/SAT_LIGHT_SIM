---
pages: [rendering/points-bloom-glare.md, rendering/refraction.md]
code: [shaders/star_point.vert, shaders/include/point_style.glsl, shaders/star_point.frag, shaders/sat_point.frag, shaders/turb_warp.frag, src/simulations/SatelliteSimTurbulence.cpp]
---
## Now
- "Star twinkle rate" governs the stars' twinkle: 0 = none at all (the amplitude ramps in to full by 0.1), above that the
  three twinkle frequencies scale with it. Each star's twinkle phase is fixed per star (hashed from its catalogue index).
- Point sprites (stars, planets, satellites) average their Gaussian PSF over the pixel, so a point keeps its drawn light
  at any sub-pixel position: no pulsing as the sky turns or the camera pans.
- The turbulence warp boils through three cross-faded noise phases (0.9 s, a third apart), which change at a constant
  rate. Copy + warp ~0.4 ms at 1920x1009 when it runs.

## History
- 2026-10-28 (issue 20261028_220009): the twinkle phase was a hash of the star's moving sky direction, a new random value
  every frame while time ran: stars flickered at the frame rate at any twinkle rate, not when paused, not above the air.
  Catalogue stars' frame-to-frame variation at rate 0 went 57% -> 1.3%.
- 2026-10-28 (issue 20261028_223915): the warp used two phases of 0.8 s, which stood still twice a cycle; it read as
  updates ~2.5 times a second.
