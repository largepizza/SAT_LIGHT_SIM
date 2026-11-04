---
pages: [rendering/clouds/march.md, development/profiling.md]
code: [shaders/include/clouds_v2.glsl, shaders/include/cv2_cdc.glsl, shaders/cloud_v2_cdc.comp, shaders/cloud_v2_march.comp]
---
## Now
Each cell of the occupancy clipmap holds, in its low 16 bits, a LAYER MASK: which of `cv2Field`'s layer functions
(storm towers, low, mid, high, anvils/shields; dust is never skipped) are non-zero or change another layer anywhere in
the cell's bake points, plus bit 64 = "some cloud" (clear = empty, the coarse skip as before). The bake records the bits
(`CV2_LAYER_HITS`, re-running the low field without the towers' proximity / rain core and the shields without the
towers' hang to see whether the towers matter there) and ORs them with `atomicOr`. The march sets `gCv2LayMask` from the
point's own cell before each view read and each light-march field read; `cv2Field` calls only the masked layers.
Unknown or outside cells, and every other shader, call all of them. No setting.

## History
2026-11-04, proposal 1 of the storm-perf write-up (issue 20261104_020257). Cloud march, RTX 3070 Ti, 1600x900, the storm
report's settings, interleaved: storm shot 1 still 14.2 -> 12.6 ms, flying east from it at 4.9 km/s 21.4 -> 19.1, shot 2
7.5 -> 7.2, cumulus from 3 km 9.3 -> 8.4, orbit 6.6 -> 6.8, the report's fast trails +0.1..+0.4. 128 registers, images
at the noise floor. Estimated -2..-4 ms at shot 1; the towers' storm strength (which cuts the mid sheet) keeps the tower
walk on in an As sheet within a tower's reach.
