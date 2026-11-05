---
pages: [rendering/clouds/march.md, development/profiling.md, development/harness.md]
code: [src/simulations/SatelliteSimCloudsV2.cpp, src/simulations/SatelliteSim.h, shaders/cloud_v2_cdc.comp, tools/harness/shader_variant.py]
---
## Now
The occupancy clipmap (empty-space skip) stays on while the eye moves anywhere inside the cloud shell (eye below the
shell top + 2 km, any height; `clouds_v2.cdc_motion_low_max_m` 30000) at up to 25% of the motion guard's 20-km scale
a frame (`cdc_motion_low_frac` 0.25, i.e. 5 km a frame); the bake brackets its footprint over the level's ring as
before. Above the shell the old guard (0.4% a frame) still drops it. The clipmap's epoch includes the low shape
volume's frame (`cv2ShapeFrame`, one of four tile rotations chosen at the observer): crossing a line where the choice
changes starts a new epoch. Measured (RTX 3070 Ti, 1600x900, High, cloud bucket): storm trail at 12 km moving 54 -> 45 ms,
anvil walk at 11 km 75 -> 67 ms, still unchanged; moving frames vs settled references: storm 1.36% -> 0.21% of pixels
> 16 levels, anvil 5.9% -> 3.3%. Harness: `shaders reload cdc=<spv>` loads a variant of the clipmap bake
(`shader_variant.py cdc`).

## History
Until 2026-11-04 the clipmap was dropped in motion above 6 km because keeping it on over storm tops lost cloud masses
(12-19% of pixels), blamed on the bake's footprint seen from another eye. The real cause was the shape frame missing
from the epoch (cells baked under the old frame read empty where the new lobes are); found with the miss counters
(`-DCV2_EVAL_COUNT`) by teleport tests at a fixed footprint (1.5 km bad, 1 km fine: the frame switched in between).
Brackets with more footprint / stretch reads, a height margin and an eye-independent S2 footprint each changed nothing.
