---
pages: [rendering/clouds/march.md]
code: [shaders/cloud_v2_lightning.comp, shaders/cloud_v2_march.comp, shaders/include/cloud_lightning.glsl, src/simulations/SatelliteSim.h, src/simulations/SatelliteSimCloudsV2.cpp]
---
## Now
**The Sun's shadow about the eye's line to it.** A cloud sample's own light march reaches about 5 km toward the Sun
(2 x the light-march length). What lies beyond that — a storm between a low Sun and the clouds in front of it, the
bumps of a deck shading each other at sunset — comes from a grid: each frame the lightning pass (workgroups 5-8)
marches 32 x 32 parallel rays toward the Sun, 250 m apart across its direction, centred on the eye, and stores the
Sun's transmittance BEYOND 17 points of each ray (`cv2SunGrid`, in the flash buffer; ice counts 0.2, as in the light
march). A sample takes the four rays around it, read at the point where its light march ends, so the two never count
the same cloud twice. The shadow fades out 3-3.9 km from the eye's line. The block is skipped when no ray found cloud.
The Sun disc, its glare, flare and bloom still use the eye's own ray (`sun_cloud_t` in harness `state`).

Each ray is the mean of four sub-rays spread over its cell, 48 steps each, every sub-ray at its own phase of the
step; the rays' length is the cloud shell's depth along the Sun's direction (at most 400 km) whatever the eye's
height, so the sample depths do not move as the eye climbs.

Cost: 128 registers in the march; within run-to-run noise on the GPU frame.

## History
Until 2026-10-10 it was one ray (review 22b's profile), applied to every sample within a 2-km Gaussian of the eye's
Sun line and read at the sample's own point. Two faults (user snapshots at 32.6 S 143.2 E, 895 m): the Sun slipping
behind the tip of one cloud darkened every cloud near the eye — 340 m of walking turned the whole cloud overhead from
white to dark blue-grey, with a hard outline — and the blocking cloud counted its own depth twice, once in the light
march and once in the profile, so the spot the Sun stood behind was the darkest part of it. With the read point at
the sample itself the grid draws every near cloud's self-shadow as hard wedges converging on the Sun.

The grid's first cut was 7 x 7 single rays 500 m apart, 32 steps, with a length of (shell top - eye height) /
sin(elevation). Over a deck at sunset (user snapshot, 35.25 N 67.41 E, 7.7 km) it drew straight-edged squares of
light and shadow that flipped with ~100 m of climb: one ray per cell hit or missed the cloud tops, and the length
changing with the eye's height slid every sample along its ray.
