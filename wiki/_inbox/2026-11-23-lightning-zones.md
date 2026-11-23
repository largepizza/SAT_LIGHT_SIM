---
pages: [rendering/clouds/weather.md, using/weather.md, using/graphics-settings.md]
code: [shaders/cloud_v2_lightning.comp, shaders/include/cloud_lightning.glsl, src/simulations/SatelliteSimCloudsV2.cpp, src/simulations/SatelliteSim.h]
---
## Now
The heavy-rain lightning source (lattice 2) draws its candidates only in pre-marked LIGHTNING ZONES. The lightning
pass's last 96 workgroups build a zone map a 16th of the globe a frame (the whole map in 16 frames): per storm bin
(6 x 256 x 256 cube-face bins of the drifted sphere, ~39 km) inside the eye's reach, up to 4 columns where the weather
map's rain is over 0.08 are probed with the deep-column gate (the raining column at least 2 km deep with cloud in it,
its top over a quarter of the cumulonimbus height); the first that passes is stored as its cloud base, top and the
map's rain, and each 156-km region keeps a 16-bit mask of its zone bins. A candidate picks a zone bin, a point in it,
and reads one field sample half way up the bin's run; the flash takes the bin's base and top. "Lightning rate: heavy
rain" is now flashes / min per 20 x 20 km of ZONE, default 4.5: the globe's flashes about double (x 2.1 over five
3780-km caps), mostly over the Peruvian Andes and coast (20-10 S) and the eastern Pacific / Mexico (10-30 N); the
lightning pass costs +0.3-0.5 ms at the worst 3780-km view (it was ~0.37 ms per unit of rate before), the ground within
noise. The Weather window's world estimate leaves this source out (the zone map covers only the eye's reach).

## History
2026-11-23 (wave 20261122 c32): c31's source probed every candidate's column and nearly all failed, so doubling the
globe's flashes would have cost 3-16 ms from orbit; the user chose to pre-mark the zones rather than loosen the gate.
