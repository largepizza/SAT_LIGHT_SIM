---
pages: ["new: simulation/crew-flights.md", modding/constellations.md, modding/satellite-models.md, rendering/meshes.md, development/harness.md]
code: [src/simulations/SatelliteSimLaunches.cpp, src/simulations/Launches.h, src/simulations/SatelliteSim.cpp, shaders/sat_mesh.frag, data/launches.json, tools/make_crew_models.py, tools/make_rocket_models.py, tools/make_station_models.py, tools/harness/mission_ops.py]
---
## Now
Cargo ships that cannot dock are berthed by the ISS's Canadarm2. A program's crew block with `"berth": true` (plus
`grapple` [radius, z below the port] and `capture_m`, default 9) flies up the R-bar to a nadir port with holds at 250 m
and 30 m, stops at the capture point and free-drifts; the arm grapples it 5 min later, moves it to 1.5 m in 50 min and
onto the CBM in 20 min (berthed). At departure the arm lifts it off, carries it back to the release point and lets go;
it then drifts off, burns and deorbits. Programs: Cygnus (Falcon 9, SLC-40, every 0.75 y, Unity nadir, 60-100 days)
and HTV-X (H3-24W, Tanegashima, yearly, Harmony nadir, 120-170 days); both burn up. The arm is data:
`launches.json` `"station_arms"` (station `match`, base and its normal in the station frame, shoulder / boom / wrist
lengths, park pose, elbow pole); its pose is a pure function of sim time (the tip follows the park, a 0.6-m stand-off
from the fixture, or the fixture itself, blended by smootherstep; two equal booms solve to the wrist point) and it is
drawn as three meshes (`rocket_arm_canadarm2_boom` twice, `_wrist`). A station with a visiting vehicle or its arm
nearby is drawn even in the Earth's shadow. Progress / Soyuz MS wings face along the vehicle's axis; Tianzhou's and
Shenzhou's turn about their span to the Sun (a "wings" child group, joint `track`). Mesh local lights (engines, floods)
are culled per instance on the CPU (`meshLightMask`) and also light the info window's 3D view.

## History
Measured (launch check, 4 cached berthing visits): arm tip on the fixture to 0.000 m at capture, berthed and release,
joints <= 0.012 m/s, booms <= 0.12 deg/s, nothing out of reach. The ISS used to carry a static Canadarm2 greeble; the
first wings of Progress / Tianzhou were edge-on to the axis (a thin line from behind). Local-light cull: launch_bench
night_close 16.2 -> ~15.5 ms GPU (runs not fully isolated from other apps). mission-ops `docking.hours_after_launch`
widened to 120 h (HTV-X berths ~4.3 days after launch, as the real HTVs). Not built: Dream Chaser, Antares 330,
local-light shadows.
