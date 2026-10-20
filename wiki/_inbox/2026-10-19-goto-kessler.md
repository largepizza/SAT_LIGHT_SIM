---
pages: [using/controls.md, using/features.md, simulation/index.md]
code: [src/simulations/SatelliteSim.cpp, src/simulations/SatelliteSimLaunches.cpp, src/simulations/SatelliteSimKessler.cpp, src/simulations/Kessler.cpp, src/simulations/Kessler.h, shaders/sat_orbit.comp]
---
## Now
**Go to: Stay here.** The follow chip at the top of the screen has a "Stay here" button (key G, "Leave Go To Here",
rebindable): it ends Go to without flying home. The camera becomes a free observer exactly where it was (position,
altitude, view and zoom kept). With time paused the satellite stays beside you; with time running it flies on. WASD and
Q/E move as for any observer at that altitude. "Exit" still flies back to the ground. Like any observer, only its
latitude and longitude are saved: a relaunch starts on the ground below.

**Go to through a whole mission.** A followed rocket is evaluated at the frame's exact time at any time warp. Followed
from lift-off it carries on with the upper stage at staging (not the booster), through the upper-stage burn, the coast,
deployment, the deorbit fall and the ship's re-entry glow; a stage that deorbits hands over to its re-entry fireball.
The camera eases across each hand-over. A raising satellite hands over to its place in the constellation on arrival, a
retiring satellite or a re-entering debris piece to its fireball. When nothing continues the object, Go to ends in place
(a free observer there) with a message.

**Explosions at any time, without waiting.** Explode / ASAT strike works at any date. Before the Sun's Fury it starts the
Kessler scenario at that moment (and switches it on if it was off); until the CME, working satellites dodge everything,
so only the debris, the dead and the passive junk collide; after the CME the configured avoidance applies. The breakup
is computed immediately (the debris is there the same frame) and time keeps running: the world is rewound to its last
6-hour step before the breakup and only that step is re-run. Playing back before the breakup undoes it.
Measured (shipped roster, x1): 30-100 ms on day 9, 229 ms on day 30, no hold; before, a breakup held time 2.4 s (day 9)
to 4.3 s (day 30) and all the debris vanished meanwhile.

## History
Until 2026-10-19 a breakup restarted the cascade from the CME (replaying every event since) and time held at the
simulated frontier; breakups were refused before the CME. Go to on a rocket drawn as a mesh could latch onto a mesh
slot holding the rocket's last state, so the camera stayed in orbit long after the rocket was gone (issue
20261019_180502); a stack followed through staging went on with the booster.
