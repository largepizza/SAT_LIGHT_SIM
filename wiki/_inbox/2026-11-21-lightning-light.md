---
pages: [rendering/clouds/weather.md, rendering/clouds/march.md]
code: [shaders/include/flash_volume.glsl, shaders/cloud_v2_flashvol.comp, shaders/lightning.frag, src/simulations/SatelliteSimLightning.cpp]
---
## Now
A lightning flash lights the clouds through the cloud around it. Each light emitter of the 16 nearest (a flash's
origin, where a ground stroke leaves the cloud, two points on its descent below the cloud, a spider's spreading
fronts) holds a small volume of the cloud's optical depth outward from it (32 azimuths x 32 elevations x 16 distances
out to 24 km), baked by `cloud_v2_flashvol.comp` when the emitter is new or has moved 600 m, at most 4 a frame and
none without a flash. `lightning.frag` adds the light after the sky TAA at full resolution with the formula the
cloud march uses for a reflector beam: irradiance by distance x (the cloud's two-lobe phase x e^-od + the slab's
diffuse field), od read from the volume. A flash inside a cloud keeps its light in that cloud (a factor N from the
depth of its own cloud in every direction, at most 40), so its thin sides glow brighter than the inverse square and
its thick sides dimmer; a flash behind thick cloud does not light the near side. Setting
`clouds_v2.lightning_light` (settings.json only, default 1; 0 = the old glow by distance alone).

## History
Until 2026-11-21 a flash added light to all cloud within a radius by distance alone (`boltGlow`): no occlusion, no
phase function (issue_20261121_163317: "an ambient source lighting clouds in all directions"). The light stays out of
the cloud march on purpose: the march's output is accumulated over frames (a 35-ms stroke would smear) and its shader
sits on a register cliff. Measured
at the report's storm (RTX 3070 Ti, 1600x900): a flash on the far side of a tower lights the near side at 0.6% of what
the old glow gave it; the light inside a lit cloud varies 3.6-11.6x between its bright and dark tenths (2.1-2.2x
before); GPU frame time with a flash on screen 29.81 -> 29.86 ms (+-0.6), the cloud march untouched (128 registers).
