---
pages: [rendering/weather/lightning.md, rendering/weather/storms.md]
code: [shaders/lightning.frag, shaders/lightning.vert, src/simulations/SatelliteSimLightning.cpp, src/simulations/SatelliteSimHarness.cpp]
---
## Now
A ground stroke is hidden by the depth attachment and by the cloud composite in front of it. In clear air at night a
stroke 3 / 6 / 12 km away draws its whole channel (44,000 / 14,700 / 3,200 px over 32 levels at 1920x1009). Five km
from a supercell, outside its rain, the composite is opaque 0.8-2 km out (the forward flank's curtain and the storm's
cloud; transmittance 0.004-0.01 at the end), so strokes 3-12 km out are hidden; under the storm in rain of 4.8 / km
(~57 mm/h Marshall-Palmer) the composite is opaque 30-90 m from the eye. The composite's opaque distance is the air
split, which near the eye agrees with the march's half-transmittance distance within a few per cent. The flash's light
on the cloud behind a stroke lifts the frame to 235-252 of 255, leaving the channel 13-37 levels at most.
"Storm frequency" 0.18 (default): 177 storms, 18.6 flashes / s worldwide; 0.30: 268, 28.8; 0.45: 403, 42.8 (the world's
~45). Cost from 0.18 to 0.45: +2.1 ms on the ground, +1.0 at 10 km, +3.3 at 540 km (the cloud march); the default stays.
Harness `lightning color knock=<bits>`: 1 no depth test, 2 nothing in front, 4 a fixed bright core, 8 a readout of the
composite in front of the core.

## History
Wave 20261122 c23: c14 / c16 had measured hand-placed strokes drawing 25-79 px in front of a supercell and suspected a
depth / occlusion defect. Knockouts show the composite and the depth test (which carry the same cloud distance) hide them,
and the readout shows that distance is where the rain and cloud are. A variant that tested strokes against the resolved
half-transmittance distance and the half-res scene depth instead changed nothing measurable and was not kept.
