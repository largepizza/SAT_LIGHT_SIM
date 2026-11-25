---
pages: [simulation/kessler.md, simulation/attitude.md]
code: [src/simulations/Kessler.cpp, src/simulations/SatelliteSimKessler.cpp]
---
## Now
The Sun's Fury takes 80% of the sun-reflecting mirrors (Reflect Orbital: 55-m, 500-kg film sails at 500 km) out of
control (`kFuryMirrorLossP`, `TypeInfo::stormLossP`, applied in `stormSeedEvents` to any type whose attitude aims a
ground site). Each is lost between 0.5 and 13 h after the CME's arrival (front-loaded), as an ordinary storm kill: it
tumbles (loose glints), aims at nothing (no beam), never avoids debris, and drag brings it down within weeks
(ballistic coefficient ~0.3 kg/m^2). Reason: the storm lifts the thermosphere at 500 km ~8x; the drag torque on the
sail (~0.3 N m with a ~1 m centre-of-pressure offset) saturates a small satellite's reaction wheels within minutes
on a face-on pass. Measured: at 2037-10-08 09:24 UTC, 999 of 5000 mirrors in control, 282 beams worldwide vs 1410
without the storm; Nuuk 75 vs 356.

## History
issue_20261124_194453 "Reflect Satellite Suns Fury". Before, the mirrors died at the generic shell rate (~1.9% at
500 km), so the storm barely touched the beams (-3.6%). kCacheVersion 22: the shipped prebake must be rebaked
(after the user's QA); until then the cascade is computed live (30 days in ~7.5 s).
