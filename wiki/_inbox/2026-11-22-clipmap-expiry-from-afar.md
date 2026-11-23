---
pages: [rendering/clouds/march.md, development/profiling.md]
code: [src/simulations/SatelliteSimCloudsV2.cpp, src/simulations/SatelliteSim.h, src/simulations/SatelliteSimUI.cpp]
---
## Now
The cloud occupancy clipmap's cells (cv2_cdc.glsl, cloud_v2_cdc.comp) expire after half a cell of 40-m/s wind slide or
half a pixel's footprint at the nearest cloud (height over the cloud shell + 20 km), whichever is longer, in powers of
two per level (`clouds_v2.cdc_far_expiry` 1, no slider; 0 = the cell alone). Below ~100 km nothing changes; from orbit
the fine levels, whose cells are a fraction of a pixel there, are re-baked far less often. Paused, nothing expires.

## History
Until 2026-11-22 every level expired at half a cell of wind slide (1.25 s for the 100-m level) at any altitude. From
1037 km with time at 1x (issue_20261122_114811 shot 1) the bake cost 0.8 ms for no march saving; with the stretched
expiry 0.42 ms (GPU 35.7 -> 34.4 ms), at 420 km 1.01 -> 0.77 ms; image at the floor (RTX 3070 Ti, 1920x1009, 3 reps).
