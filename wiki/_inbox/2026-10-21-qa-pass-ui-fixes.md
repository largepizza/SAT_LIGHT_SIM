---
pages: [using/controls.md, using/index.md, development/controls-and-settings.md, history/known-issues.md]
code: [src/simulations/SatelliteSimUI.cpp, src/simulations/SatelliteSim.h, src/simulations/SatelliteSimMissions.cpp, src/simulations/SatelliteSim.cpp, src/simulations/SatelliteSimLaunches.cpp, shaders/cloud_march.comp, shaders/sat_sky.frag]
---
## Now
- Settings > Photometry shows "Resolved satellites", "Satellite adaptation" and "Merge formations" (POINT SOURCES) and
  "Glare cluster merge" (BLOOM AND GLARE); keys `photometry.sat_resolved`, `sat_adaptation`, `formation_merge`,
  `glare_cluster_merge`, defaults 1 (they were settable only in settings.json).
- Atmosphere > aurora "Arc systems" (`clouds.aurora_arc_systems`) is 0..1, default 1, which looks as the old default 2 did
  (the storm coverage threshold reaches -0.1 at 1); a saved value above 1 loads as 1.
- Esc while a key or pad rebind is listening cancels the rebind; it no longer quits the app.
- The Missions window's action result line (e.g. "... lift-off in 45 s") disappears after 12 s.
- A uiKV value (label / value rows: Missions > Controls, the Info window's STATUS and MISSION rows) wraps onto more lines
  instead of being cut at the window's edge.
- Docking-port names read as words ("IDA-2 Harmony forward", "node port nadir"), not model ids, in the follow bar,
  Missions rows and milestones. The Info window's STATUS row for a visiting spacecraft reads "Spacecraft" / "Since
  separation" (it read "Re-entry" / "Glowing for"), for a fairing half "Fairing half".
- Go to > Stay here > Return re-follows a visiting spacecraft (Progress, Dragon, ...) still flying.
- The off-screen selection chip and the "Go to ... again" chip show the object's name only (up to its first ",", ": "
  or " ("), and the selection chip sits below the follow bar while following.
- Sky position: an object behind the Earth from an observer standing at sea level now reads "Below horizon" (the
  Info window used to show a magnitude for a satellite 67 deg below the horizon).

## History
2026-10-21 exploratory QA pass of v1.2 (fresh-install defaults, harness): the Photometry rows were in the slider table
but in no display group; the arc-systems default sat outside its slider range and the shader used it unclamped.
