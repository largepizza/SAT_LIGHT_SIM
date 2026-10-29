---
pages: [rendering/clouds/storms.md, development/harness.md]
code: [src/simulations/SatelliteSimWildfire.cpp, src/simulations/Wildfire.cpp, src/simulations/Wildfire.h]
---
## Now
The burning front close up is a ragged BAND: `wx::fireFrontWiggleM` displaces the edge by four octaves of value noise
along the arc (~35 m to ~2 km, re-drawn over 20 min to 8 h), each flame tongue stands at a hashed depth into the
flaming zone, a third have a smaller flame burning out behind, and spot flames burn ahead of the head. A resolved
tongue is a cluster of 1-4 flames (body + a tip leaning downwind and swaying), each pulsing with smooth value noise in
sim time at its width's puffing frequency (~1.5 / sqrt(D) Hz), the larger shedding a rising puff of flame. Embers are
point sprites born at the flames on 2.5-7 s cycles, rising, drifting with the 10-m wind and cooling to red. Dark smoke
puffs rise from the front near the eye (within 8 km) below the smoke streaks. Flame budget 512 Gaussians, shared
nearest first. Smoke streak Gaussians are built from the smoke's own covariance (trail length + a curtain along each
section of front + round spread + thickness) on nested nodes of fixed emission time, so they neither stretch as the
trail turns nor re-cut when a segment's piece count changes. Settings (no UI): `clouds_v2.wildfire_smoke_curtain` (1;
0 = old pieces), `clouds_v2.wildfire_scars` (true), `clouds_v2.wildfire_debug` (bits: 1 flames, 2 points, 4 smoke, 8
near smoke, 16 embers off). Harness `fires` adds `embers`, `near_smoke`, `flame_near_scale`, `flame_max_sub`,
`flame_band_nearest`, `points_nearest_m`. Cost at the report view (night, 50 m up): GPU +0.35 ms, CPU +0.9 ms.

## History
Issue 20261028_191813: the band's two symmetric licks per tongue on sine rhythms read as "Fountains of Bellagio", the
front as a dashed straight line; flames vanished when panning down because the front was placed over the ignition
point's DEM + the 18-km copy's change (~130 m above the ground 47 km from ignition) and the CPU view cull tested that
raised place before the GPU snap moved it down. Now the ground is the 18-km copy at the point, shifted near the eye to
the GPU's ground. Issue 20261028_192133: each streak's head was drawn at 0.3 of its length (a dense round dot), and
every Gaussian's width was across its segment (km-wide ovals swinging with each turn).
