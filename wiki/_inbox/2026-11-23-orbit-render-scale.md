---
pages: [rendering/hardware-tiers.md, rendering/clouds/temporal.md, development/controls-and-settings.md]
code: [src/simulations/SatelliteSim.cpp, src/simulations/SatelliteSimCloudsV2.cpp, src/simulations/SatelliteSimUI.cpp]
---
## Now
Above 30 km of eye altitude (off again below 25 km) the frame renders at min(Render scale, the orbit render scale) and
the clouds follow that scale; below, the user's render scale and full-size clouds exactly. Settings > Display "Lower
render scale in orbit" (`display.orbit_render_scale_on`, on) and "Orbit render scale" (`display.orbit_render_scale`,
80%; 100% = off); the row reads "Orbit render scale (in use)" while it applies. With the automatic render scale on,
the automatic scale wins and the orbit scale is its ceiling there. Not during the HQ photo, not on Potato /
Planetarium. From orbit the clouds' sparse march takes one ray per 4 x 4 block (`clouds_v2.sparse_block_orbit` 4) and
keeps the block while the view moves (`clouds_v2.sparse_block_orbit_moving` 1). Together, at the first-ascent views
(RTX 3070 Ti, 1920x1009, High): 48.6 -> 29.8 ms GPU at 1517 km, 46.0 -> 28.8 ms at 540 km; crossing 30 km costs one
~80 ms frame (the targets are recreated).

## History
2026-11-23 (wave 20261122 c30): the user picked option A of the first-ascent look options (c25) "on ascent". Before,
the orbit block was 3 x 3 and only while the view was still, and orbit rendered at the ground's scale.
