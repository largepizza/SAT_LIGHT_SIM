---
pages: [rendering/clouds/march.md, rendering/launch-visuals.md]
code: [shaders/cloud_march.comp, shaders/cloud_v2_march.comp, src/simulations/SatelliteSimCloudsV2.cpp]
---
## Now
Launch plumes, flames, wildfire smoke, tornado funnels and fireball trails are composited through the volumetric clouds by
depth. The cloud march records each ray's opacity profile (first contact, half opacity, last contact, transmittance); the
composite splits every Gaussian over those four depth segments and composites them front to back with the cloud, so a
funnel in a rain shaft or a plume inside a cloud is partly hidden by the part in front of it. Plumes and smoke are lit
through the clouds' Sun shadow (the Sun transmittance volume), the sky light through the deck above them and the sunlight
the deck scatters down. Cost: within noise to +0.2 ms of cloud bucket; +0.5 ms beside a big wildfire.

## History
Replaced a single front/behind switch at the cloud's mean distance (blended over +-15%): a tornado 3 km away in heavy rain
was hidden entirely, and plumes under an overcast were lit as under a clear sky. Multiple scattering in a plume is now per
Gaussian rather than of the whole stack along the ray.
