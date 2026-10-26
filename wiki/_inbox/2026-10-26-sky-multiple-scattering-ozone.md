---
pages: [rendering/sky.md, rendering/clouds.md, rendering/terrain.md]
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

The ground follows it: its sky light is the cosine-weighted mean radiance of the upper hemisphere from the same bake (the
irradiance / pi, single + multiple scattering + ozone), with no civil-twilight cut-off; "Terrain sky light" 8 (the
default) is the physical irradiance. The land's direct Sun takes the same exact path with ozone. Fog and dust are lit by
the mean of both sky hemispheres, and fog's multiple scattering is the clouds' delta-Eddington field (a slab).

## History
Before: single scattering plus a hand-made sky-lit term for air in the Earth's shadow, and no ozone on the sky's Sun
path: the twilight sky read olive / brown-grey. The first ozone model (a thin shell at 25 km x the clamped secant) was
infinite at its tangent and drew a hard-edged ghost in the twilight sky; replaced by the layer chord. The volume's
additive term needed a x1e4 scale to stay out of half-float subnormals (it drew contours). The ground's old sky light was
the zenith radiance (4-step single scattering) x 0.4 x 8: within ~4% of the physical irradiance at a 60-deg Sun, but
~1.5x too blue by day, olive at twilight and cut off at -6 deg. Fog and dust took the zenith x "Sky ambient" and the
fog the octave multiple scattering (which lit thin cloud 4-13x too brightly).
