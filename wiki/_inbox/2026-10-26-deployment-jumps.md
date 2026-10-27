---
pages: [simulation/launches.md]
code: [src/simulations/SatelliteSimLaunches.cpp, src/simulations/SatelliteSim.cpp]
---
## Now
A satellite raising or retiring from a launch is drawn on the CPU (its mesh, Go to, Track, the info readouts) from
its exact path at the current sim time: `launchOrbitElems` rebuilds its osculating elements each time it is asked,
rather than reading the Kepler record last written for the GPU. The GPU point of every member drawn as a mesh is
rewritten every frame, ahead of the usual round-robin (every 2 s / 20 s of sim time, 1200 records a frame), so the
point, bloom and glare stay on the mesh at any time warp. Harness `launch jitter` reports `live` record errors
(by age since the write) and `launch exact off` restores the old behaviour for an A/B.

## History
Issue 20261026_205544 (2026-10-26): at 1 min/s the meshed members of a Starmind train jumped at regular intervals.
The records are Kepler orbits and the path is not (the climb's ramp, the two-day ease from free flight into the slot:
up to ~0.05 m/s^2), and under warp the write budget lets records age: 10-43 m off after 20-40 s at 1 min/s, 260-1700 m
after 130-255 s at 5 min/s, each snapping back at its rewrite (several times a second of wall time). After: meshes
0.0009 m from the path, their points 0.0008 m. Far points still carry the record error.
