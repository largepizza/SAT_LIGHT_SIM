---
pages: ["new: simulation/crew-flights.md", modding/constellations.md, modding/satellite-models.md, development/harness.md]
code: [src/simulations/SatelliteSimLaunches.cpp, src/simulations/Launches.h, src/simulations/SatModel.cpp, data/launches.json, tools/make_crew_models.py, tools/make_rocket_models.py, tools/make_station_models.py, tools/harness/mission_ops.py]
---
## Now
Stations are visited by crew and cargo vehicles through one model (a launches.json program with a `crew` block).
Soyuz MS, Progress MS, Shenzhou and Tianzhou fly fast-track rendezvous: the lift-off pass (within 30 days of the
nominal one) is the first at which the station's phase lets the phasing close in a `fast_dock_h` window (Soyuz /
Progress 3.0-3.7 h or 5.6-6.4 h, Shenzhou 6.2-6.9 h, Tianzhou 2.4-3.6 / 6.0-7.0 h; the 2-orbit window is preferred if
it comes within 14 days); Dragon and Gaganyaan keep the ~1-day profile. Cargo programs: Progress resupply (Soyuz-2.1a
from Baikonur to the ISS and ROS, 3 a year each, the aft cargo ports), Tianzhou resupply (Long March 7 from Wenchang to
Tiangong, 2 a year), Cargo Dragon (Falcon 9 to the ISS, Haven-2, Axiom, Starlab, Orbital Reef, 2 a year, 28-35 days,
on the crew port the current crew is not using, half a rotation after the crews). Progress and Tianzhou burn up on
re-entry (a fireball; selectable and followable); Cargo Dragon splashes down. Docking ports carry a `use` class
("crew" default / "cargo"). If a station is lost (Kessler event log): a flight still phasing calls off the
rendezvous and comes home, one on its approach turns away, a docked crew undocks in an emergency, a docked cargo ship
is destroyed with the station. Phasing burns last 240 s (<= ~1 g); the arc onto the docking axis keeps a continuous
attitude. Entry plasma glow sits at the heat shield (`heat_shield_m`) and softens as the capsule resolves; parachute
canopies have coloured gores (Soyuz orange-white, Shenzhou/Dragon red-white) and suspension lines.
mission_ops.py now judges every cached visiting vehicle around each epoch (VISIT_THRESHOLDS).

## History
Before: no cargo flights (Progress/Tianzhou were render-only greebles on the station models); Soyuz/Shenzhou docked
8-40 h after lift-off (no launch-time phasing); phasing burns were impulsive (~84 m/s^2 in a 1-s difference); the
approach arc rebuilt its frame from an "up" reference and rolled 180 deg in a second on some arcs; a docked vehicle at
a destroyed station stayed docked to nothing. Measured after (92 cached flights, three epochs): fast dockings 3.3-6.8 h,
burns <= 13 m/s^2, turns <= 1.5 deg/s, port conflicts 0. First cut of the daylight entry sheath (50% of the point's
light) washed the frame pink; now 3%.
