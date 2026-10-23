---
pages: [rendering/atmosphere-and-sky.md, rendering/clouds/march.md, using/features.md]
code: [shaders/cloud_v2_froxel.comp, shaders/include/cloud_lightning.glsl (cv2FroxJitter, cv2GodFroxT, cv2StvTFine), shaders/sat_sky.frag, src/simulations/SatelliteSim.cpp (requestPhoto)]
---
## Now
The god rays (the clouds' and the terrain's shadows on the air) are read by the sky pass from a froxel volume of
320 x 180 screen tiles x 64 log-spaced depths (50 m - 400 km). Each frame every froxel samples the Sun transmittance
volume at its own point in its cell (an R-sequence step per frame, 2 depths), and the sky pass reconstructs trilinearly
from the same points, so the sky TAA averages the frames into a smooth result: shaft edges have no staircase and thin
shafts do not alias. Without the sky TAA the samples sit at the cells' centres (4 depths). Cost about +0.1 ms over the
earlier 160 x 90 grid. The HQ photo traces the god rays per pixel (the exact path, which the froxels approximate), so
its shafts are as sharp as the photo's resolution; a scale-2 photo takes ~6 s instead of ~4.

## History
Issue 20261023_003455 ("High res photo mode godrays", 2026-10-23): the shafts were a 160 x 90 bilinear upscale with one
ray through each tile's centre (12-px tiles at 1920 wide): a staircase along every shaft edge, the same in the HQ photo.
Error against the exact path in the report's crop fell from 2.37 to 1.51 levels.
