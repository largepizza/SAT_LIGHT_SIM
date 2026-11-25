---
pages: [simulation/kessler.md]
code: [src/simulations/SatelliteSimKessler.cpp]
---
## Now
A collision's dust cloud (kesslerFillDust) is drawn as two streams along the collision's relative velocity plus a
core at the centre of mass, not as one isotropic cloud. A catastrophic pair collision follows the engine's momentum
model (each side keeps a share beta, mean 0.68, of its velocity relative to the centre of mass; light by mass share;
75% of the light in the streams); a cratering or small-fragment hit sprays the target's ejecta forward and back along
the impactor's path (65% of the light), faster with the specific energy. While the streams are drawn the core's
isotropic kick is capped at 0.25 x the closing speed. The cone narrows with the
specific energy (ejecta 40 -> 15 deg, a parent's own stream 30 -> 10 deg). Each stream is 7 Gaussians placed by the Clohessy-Wiltshire map; the streams hand back to the tube
as they pass ~1500-3000 km or 20-30 min. Explosions stay round. Render only (no cache / prebake change). Setting
`kessler.dust_jets` (no UI, default on). Harness: `kessler state` -> `dust.clouds[].axis_ratio_major_mid`,
`major_vs_vrel_deg`, `jet_share`.

## History
issue_20261124_193940 "Sat collision dust": "Dust explodes in a spherical shape". Before, the cloud was the CW solution
for isotropic kicks only, so a young cloud was a sphere of sigma sv t whatever the impact.
Measured on the report's collision (a 0.88 km/s pair, 300 km off): principal-axis ratio 1.02 -> 2.12 at 30-60 s,
the long axis within 0.3 deg of the relative velocity; +0.43 ms GPU in that cascade view.
