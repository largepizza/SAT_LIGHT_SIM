---
pages: [rendering/clouds/march.md, rendering/terrain.md]
code: [shaders/cloud_v2_march.comp, shaders/cloud_v2_far.comp, shaders/cloud_v2_lightning.comp, src/simulations/SatelliteSim.h, src/simulations/SatelliteSimUI.cpp]
---
## Now
The clouds' sky light is the zenith sky radiance over each sample (`skyZenithAt`: a single-scattering
integral up the local vertical, each step's sunlight through the Chapman column plus ozone). It takes 12
uniform steps to the top of the atmosphere, cached per 800 m altitude level and 25 km of ray. The far cloud
layer and the rain particles' light use copies of the same function with the same step count. At twilight
(Sun about -1.7 deg) the sky-lit side of a cloud is a neutral blue-grey: linear R/G 0.96, B/G 1.23.

A cloud sample's ambient has two parts: the sky from above through the cloud over it (diffuse transmission
1 / (1 + 0.11 tau)), and light from below, 0.35 x (1 - height fraction) of the sky light, which is the
ground's: 30% of it unshaded (open country, the horizon), the rest x the column's diffuse transmission, and
70% neutral in colour. An overcast's base is grey.

"Terrain sky light" (`clouds.terrain_sky_light`) defaults to 8.0, range 0-20, step 0.25. It scales the
terrain's sky ambient (its own zenith integral x 0.4); at 8 twilight terrain follows the sky glow as the
naked eye sees it.

## History
With 6 steps one sample at ~8 km stood for the whole troposphere (deep red under a grazing Sun) and the
next, at 25 km, for the stratosphere (blue), with nothing between carrying green: from about +1 to -3 deg
of Sun every cloud's sky-lit side was magenta (linear R/G 1.7, B/G 1.9 measured at -1.7 deg) under a sky
that was not purple. A numeric replica (`harness_runs/purple/zen.py`, local) gives 1.53 / 1.72 for 6 steps
and 1.00 / 1.29 converged; 12 steps are within a few percent from +10 to -5 deg. Equal-mass exponential
strata with 8-12 samples were worse (they starve the stratosphere). The march stays at 128 registers and
the cost is within noise (15.0 vs 14.9 ms, 30.7 vs 31.0 ms GPU at the two user snapshots).
"Terrain sky light" was 1.0 with a 0-4 range.

The light from below used to be 0.35 x the zenith sky's colour, unshaded: an overcast's base was as blue and
nearly as bright as clear sky, and "Lobe occlusion (sky light)" at 1 broke it into sky-coloured blobs (bright
patches' linear B/R 1.31, now 1.18).
