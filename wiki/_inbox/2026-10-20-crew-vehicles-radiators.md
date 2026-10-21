---
pages: [simulation/attitude.md, modding/satellite-models.md, modding/constellations.md, "new: simulation/crew-flights.md"]
code: [src/simulations/SatelliteSimLaunches.cpp, src/simulations/SatModel.cpp, shaders/sat_orbit.comp, tools/make_crew_models.py, tools/make_station_models.py, data/launches.json]
---
## Now
Crew spacecraft (Crew Dragon, Shenzhou, Soyuz MS, Gaganyaan) fly as their own selectable / followable objects after
separating from their upper stage: a coast, a Hohmann raise to a phasing orbit solved so the arrival lands near the
program's `raise_days`, a raise to the station's altitude 5 km behind it, an approach in the station's frame (1 km,
onto the port's axis, holds at 400 / 200 / 20 m, contact), docked at a station model's `docking_ports` entry until the
next crew has arrived (two ports: a hand-over of 5-7 days; one port: it leaves 2-3 days before), then back-out,
departure, a deorbit burn timed to come down at sea (Dragon, Gaganyaan) or on land (Soyuz, Shenzhou: launches.json
crew `"lands": true`), entry glow, drogues at 10 km, mains at 3 km, splashdown / landing. Everything is a closed form of
sim time. Models `rocket_capsule_<kind>[_cm|_chute].json` (tools/make_crew_models.py; origin = the docking port,
+Z out of it); launches.json vehicle `spacecraft_dock_m` places the spacecraft on its stage (checked by
`make_rocket_models.py --check`); Gaganyaan flies on the new `hlvm3`. Station models carry `"docking_ports"` ([{name,
position, axis}] in the station body frame) and no longer draw visiting crew vehicles. Mission timelines list every
phase; the upper stage after separation has its own role. Joint mode `edge_on_continuous`: edge-on to the target on
one fixed solution, so a radiator turns continuously (once per orbit); `edge_on` (the smaller solution) swaps
solutions twice an orbit. Harness: `launch crew [station=] [event=] [lead=] [follow=1]`, `launch state` ->
`crew_vehicles`, `launch check` -> `crew`, `joints [sat=] [minutes=] [step=]`.

## History
Issue 20261020_214759: before, only the rocket flew; the capsule's rendezvous was not drawn and the milestones jumped
from separation to the stage's deorbit. Issue 20261020_214656: the ISS's TRRJ radiators were `edge_on` and flipped
178.7 deg between two 20-s samples 20 s after the report's shot; now 1.3 deg per 20 s (all stations <= 2.3 except two
track joints at 7 deg where the Sun passes near their axis). First cut of the crew attitudes slerped through 180-deg
turns and flipped sides frame to frame (178 deg/s); the parachute phase started with an Earth-rotation velocity jump.
