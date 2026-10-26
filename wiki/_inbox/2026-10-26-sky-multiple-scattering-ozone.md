---
pages: [rendering/sky.md, rendering/clouds.md]
code: [shaders/sat_sky.frag, shaders/sky_ap.comp, shaders/cloud_v2_sunlut.comp, shaders/include/atmosphere.glsl, shaders/include/sky_atmos.glsl, shaders/include/cloud_lightning.glsl]
---
## Now
The sky pass (atmosphere loop, aerial-perspective volume, the sea's reflected sky) adds multiple scattering: each step
takes the Hillaire 2020 Psi_ms table the clouds' sky-light bake already makes (32 x 32 by height and Sun cosine, ground
albedo "Sky ground albedo") x the air's scattering x the camera transmittance. The Sun's path to each sample is the exact
Chapman column plus ozone, a uniform layer 15-35 km (Chappuis: vertical 0.019 / 0.027 / 0.0016 at 680 / 550 / 440 nm)
crossed for the ray's exact chord; every copy of the ozone model (clouds' Sun table, far cloud layer, lightning pass)
uses it. Twilight zenith is blue-violet, the day sky a little deeper blue. Setting "Sky model (0 single / 1 multi-scatter
+ ozone)", `clouds_v2.sky_multiple_scattering`, default 1. Not in SKY_LITE / Potato / probes. Cost within noise.

## History
Before: single scattering plus a hand-made sky-lit term for air in the Earth's shadow, and no ozone on the sky's Sun
path: the twilight sky read olive / brown-grey. The first ozone model (a thin shell at 25 km x the clamped secant) was
infinite at its tangent and drew a hard-edged ghost in the twilight sky; replaced by the layer chord. The volume's
additive term needed a x1e4 scale to stay out of half-float subnormals (it drew contours).
