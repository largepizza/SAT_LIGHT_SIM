---
pages: [rendering/sea.md, "new: simulation/tropical-cyclones.md"]
code: [shaders/include/terrain_detail.glsl, shaders/include/terrain.glsl, shaders/sat_sky.frag, src/simulations/SatelliteSimWeather.cpp, shaders/include/cloud_params.glsl]
---
## Now
A tropical cyclone raises the sea on the coast to the right of its track (left in the southern hemisphere). The peak rise
at the coast follows the storm's wind over the last ~6 h: about 1.2 m at Category 1 (33 m/s), 3.3 m at Category 3
(50 m/s), 6.4 m at 70 m/s, at most 9 m, a little more for a wide storm (x (rmw / 40 km)^0.3). It spreads across the track
over a few radii of maximum wind on the surge side and along it ahead of the centre, so a coast sees the sea rise over the
~12 h before landfall and drain over the ~8 h after (Nadine 2036, Louisiana: 0.3 m 12 h before, 4.3 m at landfall). Flat
land around stands for a shallow shelf (full surge), high land for deep water (0.45). Only the sea rises: the open ocean
is drawn as before, and on the coast the shoreline moves inland by the rise over the coastal plain's slope (up to ~14 km
on the Louisiana marsh), through the lowest land only; the flooded land is shallow, muddy water with small waves, and no
beach is drawn on the flood line. Setting "Storm surge" (`clouds_v2.storm_surge`, 0-2, default 1, no UI row yet). Harness
`state` -> `weather.surge_m` (the rise at the eye) and `weather.surge`. Cost: nothing away from a cyclone; ~0.2 ms of
scene depth over a large flood at Medium.

## History
The DEM is 8-bit (~37 m steps), so it cannot say which coastal land lies below a few metres; the flood reach is estimated
from the regional mean height (the DEM's ~85 km mean) instead of the ground itself. A first cut tinted only the flooded
former land muddy, which drew the old coastline as an outline from orbit; the mud now fades out ~8 km offshore. Full-size
open-sea waves over the flooded marsh read as ocean swell; they are now depth-limited.
