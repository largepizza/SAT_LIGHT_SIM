---
pages: [simulation/weather.md, development/harness.md]
code: [shaders/lightning.frag, shaders/include/lightning_draw.glsl, src/simulations/SatelliteSimLightning.cpp]
---
## Now
A lightning flash lights the ground and the sea while it is up (lightning.frag's flash pass). A ground stroke's trunk
is a straight line light from the cloud base to the strike point (0.1 Suns x m^2 per metre: about 7 lux 1 km away,
about a full Moon at 5 km), an in-cloud flash a dimmer point on the base under it (20 Suns x m^2). The light uses
albedo 0.15 and the local up, follows the flash's intensity (it flickers with the strokes), is dimmed by the cloud in
front and the air, by rain as 0.35/km x the storm's strength, and is lifted at night like the moonlight. It scales
with "Lightning glow". It is not shadowed by terrain. Harness: `lightning ground level= line= point= rain=`.
Cost: +0.03 ms while a flash is up, nothing otherwise.

## History
2026-11-25 (issue_20261124_192804 "what would it take for them to illuminate terrain"): the cheap version of the
two costed on 2026-11-24 (the other is a light-volume box per stroke like the beams').
