---
pages: ["new: rendering/clouds/storms.md", rendering/clouds/weather.md, development/harness.md]
code: [src/simulations/Weather.h, src/simulations/Weather.cpp, src/simulations/SatelliteSimWeather.cpp, src/simulations/SatelliteSimAmbience.cpp, src/simulations/SatelliteSimHarness.cpp]
---
## Now
The storm catalog (`wx::Catalog`) builds on a persistent thread pool (`setBuildThreads`, `parallelFor`; the sim uses
3/4 of the hardware threads, at most 12; env `SATLIGHTSIM_STORM_THREADS` overrides it for A/Bs). After a sim-time jump
every missing spawn bin is cut into 512-cell ranges and built on the pool with the missing line bins, then the missing
cyclone seasons; each range appends in cell order and the ranges are joined in order, so the storms are identical to a
serial build (a pure function of sim time). A spawn cell's map -> Earth position is the drift rotation applied directly
(checked bit for bit against `Env::mapToEarth` per bin) and its land share comes from `Env::landAt` (optional, = `land`
at asin(z), atan2(y, x)). The per-frame tower / shield / envelope indices are filled into host memory and copied into
the storm buffer in one run (it is device-local host-visible, write-combined memory: scattered stores were most of the
cost), with an occupancy bitmap giving the slots in order; the envelope index's flowed copies run on the pool. Harness
`state` -> `weather.gpu_hash` / `env_hash`: a checksum of everything the storm buffer gives the GPU, for checking that
such changes leave it unchanged.

## History
Measured 2026-10-23 (Medium, Rosa's eye from 12 km, RTX 3070 Ti, 16 threads): `updateStorms` on the frame after a jump
15-24 ms -> 6-7 ms (it built ~17 spawn bins one per freshly spawned thread, at most 6, and ~8 line bins and up to 18
cyclone seasons serially); per frame at 1x 1.08 -> 0.92 ms; the envelope index 1.6 -> 1.3 ms on a frame the weather bake
runs; a bin crossing at 1 h/s 1.8-2.6 -> 0.6-0.9 ms. `gpu_hash` identical at 9 times reached by jumps both ways.
GPU, tried and not shipped: the eyewall towers cost ~2.7-4 ms of the eye view's ~12.5 ms cloud bucket, mostly the
evaluation of towers in reach (warp-coherent); cheaper per-lane rejects of out-of-reach towers (an axis + reach list:
-0.2..-0.35 ms in the eye, 0 elsewhere; 4 x 4 sub-cell masks per list entry: 0) did not pay for themselves. An
incremental table (only changed slots written) was slower (scattered write-combined stores), and caching the index when
its entries repeat never hit (some tower changes bin or order every frame).
