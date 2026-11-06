---
pages: [rendering/clouds/march.md]
code: [shaders/cloud_v2_march.comp, shaders/include/clouds_v2.glsl, src/simulations/SatelliteSimCloudsV2.cpp]
---
## Now
The diffuse field's far side (tauB: how much cloud lies behind a lit sample, away from the light) is two field reads
jittered over a neighbourhood -- +-450 m along the local up and +-300 m across the light (`kCv2BackJitZ` / `kCv2BackJitH`
in cloud_v2_march.comp), the second read mirroring the first -- combined through the gamma model over `ms_back_m`.
It is the same at every distance, time rate and camera speed, so the shaded grey-blue lobes and crevices of front-lit
cumulus (what users call the droplet-phase look) show on every cloud, near or far, still or moving. The low shape
volume's frame (one of four tile rotations chosen at the observer) cross-fades over 90 frames when the choice changes
(UBO `shapeB[5]`: the old frame's rotation and anchors and the share of view rays still taking it; each ray sees one
frame whole and the history blends the two images); the occupancy clipmap is off during the fade, a frame in use is
kept while still clear enough (hysteresis), and a teleport switches at once. The far side's field reads cost +0.2-0.9 ms
of cloud march over cumulus and +1.2-1.6 ms in a storm view (RTX 3070 Ti, 1920x1009).

## History
Until 2026-11-06 tauB came from the occupancy clipmap's cell moments within `cdc_light_levels`' reach (~15-25 km) and
from two point reads elsewhere, which inside a cumulus always find dense cloud: clouds past ~20 km, clouds under time
warp past `cdc_max_rate` (4x) and clouds during the clipmap's refill rendered flat white, and the shading's scale
jumped at each clipmap level. The shape frame switched in one frame, so flying across a switch line reshaped every
cloud in view (issues 20261105_202810, 20261105_203119).
