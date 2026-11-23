---
pages: [rendering/clouds/weather.md, using/weather.md, using/graphics-settings.md]
code: [shaders/cloud_v2_lightning.comp, src/simulations/SatelliteSimCloudsV2.cpp, src/simulations/SatelliteSimUI.cpp]
---
## Now
Lightning has a third source beside the storm objects' towers and storm regions: HEAVY RAIN away from any storm
(cloud_v2_lightning.comp lattice 2). The storm regions' 64 x 64 cube-face lattice (~156 km) draws a second set of
candidate flashes over the weather map's rain: "Lightning rate: heavy rain (/min per 20 km)"
(`clouds_v2.lightning_rain_rate`, Settings > Weather > Lightning, slot 630), kept with the map's rain over a threshold
(none below 0.08, all at 0.3), never inside a storm tower's reach, with an ordinary storm's cloud-to-ground share. A
candidate flashes only in a deep raining column the field actually draws there: one run of cloud and its rain at
least 2 km deep, with cloud in it, topping out above a quarter of the cumulonimbus height (~3-4 km); the flash sits
in that run's cloud. No storm object and no cloud is added, so the cost is the lightning pass's alone. A region is
walked only when a coarse read of the map's rain in it is over 0.05 (the storm regions' mask idea). It flashes in
tropical and monsoon rain; mid-latitude frontal rain mostly does not (its drawn decks stay low). Default 2.0 (slider
0-10): the lightning pass's cost from high orbit is linear in the rate (~0.37 ms a unit over the rainiest 3780-km cap
measured, RTX 3070 Ti), on the ground within noise.

## History
2026-11-23 (wave 20261122 c31): the user asked for more lightning without more storms (storm_frequency 0.45 gave ~43
flashes/s but cost 1-3.3 ms). The storm regions' own deep-cloud test (cloud alone, top over 0.55 of the Cb height)
passed none of the rain candidates: the field draws the map's rain as a ~1-km deck with rain under it, so the rain
source's test counts the rain in the column.
