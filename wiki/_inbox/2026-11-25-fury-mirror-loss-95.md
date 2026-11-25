---
pages: [simulation/kessler.md, simulation/attitude.md]
code: [src/simulations/SatelliteSimKessler.cpp, src/simulations/Kessler.cpp]
---
## Now
The Sun's Fury takes 95% of the sun-reflecting mirrors out of control (`kFuryMirrorLossP` 0.95; it was 0.8 on
2026-11-24, see 2026-11-24-fury-mirror-loss.md). Only a mirror already feathered edge-on to the flow, or in the Earth's
shadow and safed, when the density surge arrives keeps control. Measured at 2037-10-08 09:24 UTC (day 85): 232 of 5000
mirrors in control, 66 beams worldwide vs 1410 without the storm; Nuuk 17 vs 356. A lost mirror makes no beam, ground
spot, cloud light or shaft; it only glints as it tumbles.
A beam's ground irradiance is E = A F cos(i) / (pi R^2) Suns, R = mirror radius + slant x tan(0.2665 deg): 1.4e-5 Suns
(~1.8 lx) for the 256-m^2 film at 500 km; a site's ground light, cloud light and shafts are linear in the sum of its
beams' E (x "Beam light on cloud" 29 on display).
The shipped prebake was rebaked for it: data/kessler/suns_fury_b3dd558186539a35.pak (33.7 MB, 1100 days, ~16 min to bake).

## History
User request 2026-11-25 after liking the 80% effect. Ground tracks of tumbling mirrors' sun spots: a costed proposal in
docs/subsystems/orbits.md, not built.
