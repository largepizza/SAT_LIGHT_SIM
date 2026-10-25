---
pages: [development/profiling.md, using/missions.md, using/features.md]
code: [src/simulations/SatelliteSimWeatherFeatured.cpp, src/simulations/SatelliteSimLaunches.cpp, src/simulations/Weather.cpp, src/simulations/SatelliteSimWildfire.cpp, src/Log.cpp, src/App.cpp]
---
## Now
A Watch button lands without CPU stalls: the frames after a jump are bound by the GPU (the landing frame's CPU is
~5-13 ms). The Weather window's forward event search (tornadoes, waterspouts, haboobs) runs on a worker thread, the
weather radar's pipeline is made during loading, the storm catalog keeps its calibration when only the cloud map's
drift or coverage changes, the launch schedule hands every build after a jump to its background job and the Missions
window's own refresh builds on a worker, and wildfire points are built in parallel (the same points in the same order).
Hitch log lines are written by a writer thread (`Log::lineDeferred`). The Weather and Cinematics windows now capture the
mouse: a click in them no longer also picks the satellite behind them.

## History
Measured with every Watch clicked through the UI (harness `ui click`, the user's settings, 1600x900, RTX 3070 Ti),
worst frame before -> after: tornado 319 -> 45 ms (the radar pipeline built on first use), the Sun's Fury 101 -> 37
(catalog recalibration ~55 ms + launch builds ~20-30 ms), cyclone row 63 -> 41, haboob 103 -> 77 (now all GPU),
hurricane 57 -> 33, waterspout 50 -> 35. The featured search had a 2.5-ms budget but one 30-min step on the main thread
took up to ~12 ms. Close to a big fire the wildfire fill went 1.95 -> 0.95 ms a frame. A run of slow frames paid
~5-10 ms each for its own fsynced hitch line ("app.other"). hitch_check's fast-warp failures (1 h/s and up) are the
GPU (`app.wait_fence` 45-70 ms), before and after.
