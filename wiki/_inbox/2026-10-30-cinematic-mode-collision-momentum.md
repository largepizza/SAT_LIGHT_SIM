---
pages: [using/controls.md, simulation/kessler.md]
code: [src/simulations/SatelliteSimUI.cpp, src/simulations/SatelliteSim.cpp, src/simulations/Kessler.cpp]
---
## Now
Cinematic mode (Movement popup): mouse motion over the UI, in a text field or with the left button down does not
turn the camera; the look keeps its velocity until you steer against it (no drag); the mouse wheel zoom eases in
over about a third of a second.

A catastrophic collision between two objects sends its fragments off at the pair's centre-of-mass velocity (80%
of the way from each parent's own velocity) plus the ejecta kick, so debris carries the impact's momentum.
The default Sun's Fury history changed with it (cache version 18).
