---
pages: [rendering/clouds/temporal.md]
code: [src/simulations/SatelliteSim.h, src/simulations/SatelliteSimUI.cpp]
---
## Now
The far cloud layer's pass (cloud_v2_far.comp) runs only once its share of the image (`cloudFarBlend()`) passes
`clouds_v2.far_layer_min_share` (0.03, no slider; 0 = from "Far cloud layer from (km)" up). Below the threshold the share
is 0; above it the share is rescaled, so it is continuous and still reaches 1. With the default range 600 .. 8900 km the
pass starts at ~1450 km. The far layer is cross-faded with the march, which draws the same clouds, so at a share s the
image moves by s x (far - march): under a level for a few percent.

## History
2026-11-09 (issue 20261109_184851) the threshold was 0.004 (`far_layer_min_blend`, pass from ~905 km). 2026-11-22
(c17 perfpass, issue_20261122_114811's view at 1037 km, share 0.004): 0.03 under a new key, so saved settings carrying
the old key take the new value; 35.4 -> 33.5 ms GPU there (RTX 3070 Ti, 1920x1009, 3 interleaved reps), image diff at
the floor (mean 0.26 vs 0.35 between two captures of the old setting).
