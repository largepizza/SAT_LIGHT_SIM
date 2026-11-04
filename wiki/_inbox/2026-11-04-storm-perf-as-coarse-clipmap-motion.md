---
pages: [rendering/clouds/march.md, rendering/clouds/storms.md, development/profiling.md]
code: [shaders/cloud_v2_march.comp, src/simulations/SatelliteSimCloudsV2.cpp, src/simulations/SatelliteSim.h]
---
## Now
The cloud march is one walk through the whole cloud shell and `cv2Field` sums every layer (towers, low, mid, high,
anvils, dust) at each sample; storms cost more than ordinary clouds because their layers are translucent and stacked
(4.85 km between a storm's decks: 6.9 lit samples a ray against 1.0 over cumulus). The mid layer's altostratus sheet
(soft, deck) is shaded at coarse steps as one sample of two steps, like cirrus and rain (it has no erosion, so the coarse
read is the fine one). The occupancy clipmap's motion guard is looser with the eye low in the cloud shell (below `clouds_v2.cdc_motion_low_max_m`,
6000 m, and the shell top + 2 km): it tolerates steps up to `clouds_v2.cdc_motion_low_frac` (0.05 of its 20-km scale: 1 km a
frame) and starts a new epoch after `clouds_v2.cdc_motion_low_epoch` (4) x that scale of travel or a quarter of it in height;
the clipmap's bake then reads the field at its level ring's nearest and farthest footprints and keeps the larger, and cells
the bake budget refuses are marked unknown. No UI; frac 0 = the old guard. Higher up the guard is unchanged.

## History
Issue 20261104_020257 "Performance in storms" (sub-20 FPS moving in storms, RTX 3070 Ti, 1920x1009, High, FOV 120): the
reported 47.5 ms cloud march was a moving frame. Flying at 4.9 km/s from the report's pose the guard kept the clipmap off
(26 M coarse field evaluations a frame instead of 5 M): 51.7 ms, guard off 36 ms; still 22.2 -> 16.5 ms with the As change, flying 4.9 km/s ~50 -> ~28 ms. At 12 km over storm tops keeping the
clipmap on changed 12-19% of the pixels (mostly its S2 light moments), hence the height limit.
Final before/after in docs/subsystems/clouds.md ("Storms between decks").
