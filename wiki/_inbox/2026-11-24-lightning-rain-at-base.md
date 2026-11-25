---
pages: [simulation/weather.md, development/harness.md]
code: [shaders/lightning.frag, shaders/lightning.vert, src/simulations/SatelliteSimLightning.cpp]
---
## Now
The through-rain floor (35% of a channel's light) also covers channel pieces up to 600 m above the cloud base
(`lightning color rain_above_m=`), seen from under the base: per pixel, the piece is floored only when everything the
cloud composite has in front of it begins before the eye's ray reaches the base (the rain under it); cloud starting
at or past the base hides it as before. The flash record carries the base height for this (BoltFlash.b, 160 bytes).

## History
2026-11-24 round b (issue_20261124_192804 shot 3): an in-cloud flash running along the base 6 km out was hidden by the
rain in front; its peak went 0.084 -> 0.44 against 0.80 with nothing in front. Keeping rain apart from cloud in the
march was not done (it would add a channel through the march, resolve and composite).
