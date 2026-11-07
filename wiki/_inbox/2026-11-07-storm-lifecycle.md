---
pages: [weather/storms.md]
code: [src/simulations/SatelliteSimWeatherSystems.cpp, shaders/include/clouds_v2.glsl, shaders/cloud_v2_weather.comp]
---
## Now
A storm cell grows out of the cumulus: its drawn rise starts when its column first appears (strength past 0.35), its
margin comes in from out of the billows' reach and its foot widens as it climbs, and the column widens by one core
sigma as it rises (a broad cumulonimbus, not a stalk). Its anvil is fed from a smooth, deterministic centroid of its
anvil towers (no jump when the last tower ends), has a lobed outline (noise on its spread radius, seeded per storm and
evolving with age), thins into fibrous cirrus toward its edges, and dissolves over the last half linger of its storm's
life instead of vanishing when the catalog drops the storm. A dying tower's billows soften, fray and thin.
Validated on harness videos at 5 min/s (`tools/harness/vidsheet.py`): the report's ITCZ from the sea, from 6.7 km and
from 420 km, and a catalog supercell; the largest frame-to-frame change fell from 9x to ~2x the median, 0 spikes from orbit.

## History
Until 2026-11-07 a cell popped in as a 3-5 km mass, its anvil jumped ~20 km when its last tower ended and vanished in
one frame at its storm's catalog end, and every anvil from orbit was a perfect disc.
