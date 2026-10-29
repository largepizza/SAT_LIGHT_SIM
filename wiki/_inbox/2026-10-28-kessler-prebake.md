---
pages: [simulation/kessler.md, development/tools.md, development/harness.md, using/features.md]
code: [src/simulations/Kessler.cpp, src/simulations/SatelliteSimKessler.cpp, tools/make_kessler_prebake.py, cmake/PackageRelease.cmake]
---
## Now
The default Sun's Fury history ships prebaked: `data/kessler/suns_fury_<hash>.pak` (copied to `<exe dir>/kessler_prebake/`),
the engine's events for 1102 days past the CME (to 2040-07-21), 31.3M events in 45 MB. The format stores per 6-h step only
the grid hits' targets (Rice-coded) and the pair collisions; times, seeds and the grid hits' projectile speed and mass are
recomputed at replay with a per-step check, so a replay is identical to live sampling (checked with harness
`kessler fingerprint`). It is used when the world hash matches and the player has made no breakup; otherwise the
existing cache / live paths run. A replay applies the events only, with each breakup's outputs prepared on all cores but
one. Jump past the CME, time to a usable view: supernova Watch (day 163) 416 s -> 41 s, 2038-06-01 652 -> 62 s,
2039-01-01 901 -> 93 s; at 1 week/s time is no longer held through the prebaked range. Rebuild with
`python tools/make_kessler_prebake.py` (~20 min) when the hash changes. Harness: `kessler fingerprint`, `kessler savecache [out=]`.

## History
The cache (user data, `.bin`, whole 80-byte events) existed but was never shipped: a fresh install computed the cascade
live (~1.1-2.5 s per simulated day) and held sim time at the frontier. Replaying a full-event cache was still apply-bound
(~2 min to day 163); the parallel prepare/commit split and the persistent thread pool took it to ~40 s. Deflate gained
nothing on the compact log (stb's fixed Huffman; zlib 12%), Rice coding 25%. Snapshots of the world were rejected: one
is ~300 MB. Also fixed: the mirror's history prune never ran while time stood still (a long wait held ~33 GB).
