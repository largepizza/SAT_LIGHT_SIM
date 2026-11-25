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
(about 5.4 magnitudes) and deep red; partial phases sit between. The light never drops below
`clouds.moon_eclipse_floor` (0.04 of full Moon, 2026-11-25) so the clouds at totality stay faintly red-lit; the disc
is unchanged. The disc's red refracted light is brightest at the umbra's edge and dims to 0.45 at its centre.

## History
2026-11-24, issue_20261124_190627 "Moonlight during blood moons": the disc went red but the clouds and sea stayed lit
grey-white as by a full Moon (the key light ignored the eclipse). 2026-11-25: at the disc's own factor the night went
black (frame mean 56 -> 4 levels): the floor (sheet 2 / 4 / 8%, 4% chosen). The umbra's depth term was inverted
(red full at the centre, 0.45 at the edge) until then.
