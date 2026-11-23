---
pages: [development/profiling.md]
code: [src/simulations/SatelliteSimLaunches.cpp]
---
## Now
Starship's steady orbital vent draws each chord of its fan as a plume Gaussian; in orbit each also gets rim
Gaussians (a limb-brightened shell). Within about 4-9 sigma of the eye the vent's rims fade out and their share goes
back to the core Gaussian, because from inside the fan there is no limb to see. Setting `launches.vent_rim_lod`
(default true; false = rims everywhere, for A/B). Measured at 1920x1009 on an RTX 3070 Ti, the Starmind deploy view
(eye 300 m from the ship): GPU 21.9 -> 19.1 ms (cloud composite 4.95 -> 1.93 ms), image at the A-vs-A floor.

## History
Wave 20261122 c22. The plume bricks' slot choice keeps whichever groups took the slots first (x1.3 hold), so the
twilight launch view costs 8-9 or 11-12 ms of composite depending on what the session saw before; both picks are valid
and left as they are. A 10x looser per-texel faint skip in cloud_march.comp was tried and saved nothing.
