---
pages: [rendering/clouds/weather.md, rendering/clouds/field.md, "new: rendering/clouds/storms.md", development/harness.md]
code: [src/simulations/Weather.h, src/simulations/Weather.cpp, src/simulations/SatelliteSimWeather.cpp, shaders/include/cv2_storms.glsl, shaders/include/clouds_v2.glsl, shaders/cloud_v2_weather.comp, src/simulations/SatelliteSimWeatherCyclones.cpp]
---
## Now
Every storm object carries an anvil SHIELD: a capsule spread downwind by its anvil-reaching towers that lingers 35-240 min
after them (drifting at the storm's upper wind, thinning and fraying into cirrus), stored in the storm buffer with its own
64-per-face bin index and drawn by the anvil shield code instead of the weather map's cumulonimbus typing. Its outline is
frayed by the cluster field and two storm-lobe octaves. "Anvils" (`clouds_v2.anvil`) defaults to 8 with the user's anvil
tune (thickness 3.7 km, hang 0.87 km, puffiness 0.45, lift 230 m, head lobes 0.12, lobes 0.37, waist 0.89, overshoot 0.14
km, storm erosion 0.24). Squall lines (180-520 km lines of regenerating towers, fast, a trailing stratiform region and
shelf-cloud band stamped into the weather cube) and MCSs (broad arcs under one round shield) come from their own spawn
pass: "Storm complexes (x)" `clouds_v2.storm_complexes` 1. Tropical cyclones (v1): six basins, ~82 a year, Earth-fixed
tracks recurving poleward, intensity toward a per-storm potential that collapses over land and cold water, named from the
WMO lists; drawn as an eyewall of towers, a central dense overcast with an eye and spiral rainbands in the cube; their
wind field drives the surface wind (rain, snow, ambience). "Tropical cyclones (x)" `clouds_v2.storm_cyclones` 1. Tornadoes
(~35% of supercells, 5-25 min) and waterspouts are plume Gaussians in the dust list: a dense narrow funnel, wall cloud and
debris / spray. The Weather window lists named storms with tracks and milestones. The storm update costs ~0.8 ms CPU a
frame (one catalog scan, packed bin times, a 4-atan bin listing).

## History
Fed by the map, shields stood over regions with no tower ("stranded anvils", issue 20261023_002905) and kept ~2% density
at their cutoff, which at Anvils 8 drew a hard arc along a view grazing the edge. The anvil used to be only each tower's
flared head and died with it (issue 20261023_001522). The first shields were clean capsules: white pills from orbit (the
cluster fBm spreads only +-0.06, so its fray needed x6). Storm update 1.8 -> 0.8 ms: two scans of ~1.3-KB storm records
a frame (cache misses) and ~68 atan per binned object.

Eyewall: 18 regenerating towers at the radius of maximum wind; they do not flare (the CDO shield is the anvil), which cut the 12 km eye view's cloud cost at Medium from 15.2 to 12.5 ms.
