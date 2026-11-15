---
pages: [development/ui.md]
code: [src/simulations/SatelliteSim.cpp, src/simulations/SatelliteSimCamera.cpp]
---
## Now
Q / E (KB_RAISE_ELEV / KB_LOWER_ELEV, and the gamepad's analog triggers gpElevRaise / gpElevLower) follow the same
rule as roll: while the horizon auto-levels (`camera.auto_level` on and the eye below `camera.auto_level_km`,
default 1000 km) they are the pure altitude move; with the horizon free they move the eye along the camera's up
(`camUp`), in double, with the ground as a floor, and the look and up are carried to the new place (`camSetEyeDir`).
Between auto_level_km and 1.25 x it (`kCamVertBlend`) the direction is a smoothstep blend of the local up and
`camUp` (`camVertFreeWeight()`), so it turns continuously as the eye crosses the limit; the toggle switches it at
once. Speed is the old Q / E rate (Move speed x (10 m + half the height above the ground) per second).

## History
Issue_20261115_133412 (2026-11-15): from 2900 km looking down, Q / E only changed the altitude (the Earth zoomed);
the reporter expected them to move along the view's up (north / south there). The user decided Q / E follow the
camera's up whenever auto-level is off.
