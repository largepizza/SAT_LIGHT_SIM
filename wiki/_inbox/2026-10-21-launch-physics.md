---
pages: ["new: simulation/launches.md"]
code: [src/simulations/SatelliteSimLaunches.cpp, src/simulations/Launches.cpp, data/launches.json, tools/harness/mission_ops.py]
---
## Now
A released train member flies free for 6 h as Clohessy-Wiltshire motion about the upper stage's parking circle (one reference orbit for the whole train), then eases into its own path by two days. Every member gets the same in-plane push (Starship door / flat stack straight up, dispenser point 0.97 up 0.22 back, column forward), differing only across the plane, with a -2 n x0 along-track trim; the deploy interval is raised so consecutive members stay >= 30 m apart (Falcon 9 dispenser ~67 s, Gen1 ~100 s). A booster arriving faster than 2.4 km/s starts its entry burn up to 100 km and burns until its peak drag stays near 1.5 g (New Glenn peaks ~4.6 g). The upper stage's deorbit burn is a flat thrust (1-s ramps) with the entry ellipse's apoapsis at the burn's middle. Booster legs: launches.json `leg_m` (Falcon 9 1.68, New Glenn 2.36, ...) lifts the touchdown so the feet stand on the deck / pad / ground. The launch a jump framed lists its stages at any range.

## History
Mission-ops suite (2026-10-21): Falcon 9 trains closed to 0.8-1.6 m (drift linearised about each member's own plane + random spread); New Glenn's booster peaked at 92 m/s^2; the smoothstep deorbit blend peaked at ~4x dv/D (~90 m/s^2); booster feet sat 1.7-2.4 m below the deck.
