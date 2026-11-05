---
pages: [rendering/atmosphere-and-sky.md, using/features.md]
code: [shaders/sat_sky.frag, shaders/cloud_v2_lightning.comp, src/simulations/SatelliteSim.cpp]
---
## Now
"White balance" (Settings > Photometry > Exposure, `clouds_v2.white_balance`, 0.6) divides the frame by the colour of the
Sun's light at SEA LEVEL under the eye (Chapman Rayleigh + Mie columns, the sky's Rayleigh gain), at any eye height up to
60 km, blending to the Sun at the eye by 200 km (from orbit the frame spans the terminator). It is full down to "White
balance holds to" (`clouds_v2.white_balance_hold_deg`, 6 deg of Sun) and off by ~1 deg, so the last degrees before sunset
keep their orange. "White balance from the eye" (`clouds_v2.white_balance_at_eye`, 0) = 1 uses the Sun at the eye (the
old reference; with hold 11.5 it is the old behaviour). The rain particles' light (cloud_v2_lightning.comp) undoes the
same factor.

## History
2026-11-05 (light2, user: clouds "pick up this off-yellow" at a low Sun; from above "a sea of off-yellow red clouds"):
the reference was the sunlight at the eye, which swings with a kilometre of the eye's height near sunset, so it was
faded out from 11.5 deg down to 1 deg (exactly where the light yellows) and from an aircraft it was white (no
adaptation of the clouds below). Lit-face blue/red at the user's 0.6, from the ground: Sun 5 deg 0.48 -> 0.74, 6 deg
0.60 -> 0.90, 8 deg 0.85 -> 1.03; >= 10 deg unchanged. Measured on the way: the cloud's sunlight transmittance is exact
(per-sample height and path; 3-digit match to a brute-force integral), and the "grey" is the tonemap's (scene lit/shaded
23.6:1 at Sun 5 deg, display 3.1-3.4:1 under AgX contrast 1.15).
