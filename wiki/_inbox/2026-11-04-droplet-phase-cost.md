---
pages: [rendering/clouds.md]
code: [shaders/cloud_v2_march.comp]
---
## Now
The droplet phase (`clouds_v2.droplet_phase`, on) costs 0.3-0.7 ms of the cloud march (2-5%) inside a storm between
its decks (issue_20261104_020257 shot 1: 14.46 ms on / 13.73 ms off, RTX 3070 Ti, 1600x900, user settings) and is
within the noise on a cumulus view (9.33 / 9.25 ms). No single part of it (Mie row fetch, low orders, HG + Draine
shape, diffuse field) costs more than ~0.15 ms. It does not change the march's steps, so the altostratus sheet's
coarse steps hold with it on.

## History
A first measurement read ~10-13 ms for it; that was the ordering of the A/B (the "on" frames were timed 5 frames
after a shader reload or a cloud-field setting change, i.e. during the history drop and clipmap refill). Re-measured
settled and interleaved on 2026-11-04 (details: docs/subsystems/clouds.md "The droplet phase").
