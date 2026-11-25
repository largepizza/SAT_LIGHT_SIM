---
pages: [rendering/moon-and-eclipses.md]
code: [src/simulations/SatelliteSim.cpp, src/simulations/SatelliteSimStars.cpp, shaders/sat_sky.frag, shaders/cloud_v2_march.comp, shaders/include/cloud_params.glsl]
---
## Now
A lunar eclipse dims and reddens the Moon's light, not only its disc. Each frame near an eclipse the CPU integrates
the disc's own eclipse shading (per surface point the Sun's disc past the Earth's, enlarged 1/85 for the atmosphere,
plus the umbra's refracted red) over the visible face into an RGB factor vs the uneclipsed Moon. Its luminance scales
the Moon's light (`moonDirENU.w`) for everything it lights (clouds, ground, sea, moonlit sky, halo, star dimming);
its colour (`CloudParams.moonTint`) tints them. At the 2037-01-31 totality the factor is ~0.007 of full Moon
(about 5.4 magnitudes) and deep red; partial phases sit between.

## History
2026-11-24, issue_20261124_190627 "Moonlight during blood moons": the disc went red but the clouds and sea stayed lit
grey-white as by a full Moon (the key light ignored the eclipse).
