---
pages: [rendering/sky.md, rendering/clouds.md, rendering/terrain.md, development/profiling.md]
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

Under cloud the ground's sky light comes through the cloud: a map of the cloud over the ground round the eye (five nested
32 x 32 levels, 1-256 km cells, one baked a frame from two vertical columns per cell, running means) gives the sky light's
diffuse transmission and the sunlight the cloud scatters down; under a thick deck the shadowed ground is neutral grey
instead of clear-sky blue. Setting "Ground light under cloud (0/1)", `clouds_v2.ground_overcast`, default 1 (needs the
sky model). Cost ~0.1-0.15 ms.

Clouds below a deck take the same map's cloud above their height: their sky light comes through the deck and the
sunlight it scatters down lights them from above, so cumulus under an overcast are neutral grey instead of clear-sky
navy. Cost ~0.3-0.4 ms of cloud march.

Performance: faint cloud samples (optical depth under 0.02 over the light march) skip the light march, and the sky pass
skips shading ground hidden behind opaque cloud; together -0.3 to -1.1 ms of cloud march, up to -0.9 ms of sky pass.

## History
Before: single scattering plus a hand-made sky-lit term for air in the Earth's shadow, and no ozone on the sky's Sun
path: the twilight sky read olive / brown-grey. The first ozone model (a thin shell at 25 km x the clamped secant) was
infinite at its tangent and drew a hard-edged ghost in the twilight sky; replaced by the layer chord. The volume's
additive term needed a x1e4 scale to stay out of half-float subnormals (it drew contours). The ground's old sky light was
the zenith radiance (4-step single scattering) x 0.4 x 8: within ~4% of the physical irradiance at a 60-deg Sun, but
~1.5x too blue by day, olive at twilight and cut off at -6 deg. Fog and dust took the zenith x "Sky ambient" and the
fog the octave multiple scattering (which lit thin cloud 4-13x too brightly). Under an overcast only the direct Sun was
shadowed, so the ground took the clear sky's blue light. A first cut of the cloud map stored the cells' mean optical
depth: it turned scattered cumulus into a uniform thin sheet and brightened the ground between clouds ~40%; it stores the
means of the columns' transmissions instead.
