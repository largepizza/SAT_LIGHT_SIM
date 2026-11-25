---
pages: [rendering/aurora-airglow.md]
code: [src/simulations/SpaceWeather.cpp, src/simulations/SatelliteSim.cpp, shaders/cloud_v2_march.comp, shaders/sat_sky.frag, shaders/include/cloud_params.glsl]
---
## Now
The aurora lights the clouds and the ground. Once a frame the host integrates the oval's column brightness over the
sky (green 557.7 nm as a layer at 120 km, red 630 nm at 280 km: the curtains' red tops and the storm-time red glow
equatorward of the oval) into the irradiance from above at the observer (`auroraSkyLightCpu`, the h/r^3 kernel,
256 band evaluations). The result, RGB in units of the full Moon overhead, joins the night sky light on the clouds'
ambient, the far layer, lightning, the sea reflection and the ground (`CloudParams.auroraLight`). Brighter storms
grow as the activity brightness squared; in a great storm the green's share falls to 0.4. Sun's Fury report view:
0.23 of a full Moon, red-orange; Kp 5 overhead at high latitude ~0.03 green (a third of a quarter Moon); an aurora low
on the horizon ~0. Faded out above 20-100 km of eye height. Settings key `clouds.aurora_light` (1). No GPU cost.

## History
A per-pixel green ground glow (auroraGlowAt, C16) was removed 2026-09-30 ("looks green and bad"); the clouds' local
aurora ambient went with clouds v1. Issue_20261124_232008 "Aurora as key light?" (2026-11-25) brought the light back
as a sky-integrated ambient, red-dominant in storms, not a key.
