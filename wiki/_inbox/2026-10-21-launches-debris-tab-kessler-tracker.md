---
pages: [using/ (the Missions window / settings pages), simulation/kessler.md, simulation/launches.md, rendering/launch-visuals.md, development/controls-and-settings.md, development/harness.md]
code: [src/simulations/SatelliteSimKesslerTracker.cpp, src/simulations/SatelliteSimMissions.cpp, src/simulations/SatelliteSimLaunches.cpp, src/simulations/SatelliteSimKessler.cpp, src/simulations/SatelliteSimUI.cpp, shaders/cloud_march.comp]
---
## Now
Settings has a **Launches & debris** tab (index 15, under SKY after Constellations): LAUNCHES (launches on, deployment
era, design life, after the Sun's Fury, watch buttons, counts), THE SUN'S FURY (scenario, collision rate, avoidance,
horizon, watch a re-entry, undo my breakups), then GRAPHICS: ROCKETS AND EXHAUST and GRAPHICS: DEBRIS AND RE-ENTRIES.
settings.json keys unchanged. The exhaust is tuned with eight sliders, 1 = the tuned look, "Reset exhaust to defaults":
Plume opacity `launches.plume_gain` 0-5, Plume brightness `plume_brightness` 0-5, Plume width `plume_width` 0.25-4 (same
cross-section, so a wider plume is thinner), Plume billows `plume_billows` 0-3 (contrast of the noise in cloud_march),
Plume growth `plume_growth` 0-5 (turbulent growth the parcels are built with; a change rebuilds the plumes in view),
Ground clouds `ground_cloud_gain` 0-5 (lift-off steam, landing dust / spray), Flame brightness `rocket_gain` 0-10,
Rocket light on clouds `engine_light_gain` 0-10.

The Missions window's third tab is the **Kessler** tracker: a Sun's Fury strip (countdown / DAY n), big counts
(collisions, catastrophic, pieces >= 1 m, 10-cm fragments made, dead, destroyed, re-entered, still working), a small
globe seen from the equator with the debris shells drawn round it as a polar plot (radius = altitude x1.6, angle =
latitude), blue where working satellites fly and a heat scale of the chance a year that a 10 m^2 satellite there is hit
by a tracked object (dead satellites + pieces >= 1 m; density x 10 km/s x 10 m^2 x a year), numbered markers for the 8
latest significant breakups (drag the globe to turn it), the collisions per day since the CME, and the breakups listed
with Watch (8 s before at 1x, 80 km beside the orbit). All from the counts walk spread over frames; the globe is a CPU
raster re-made only when its inputs change.

## History
Issue 20261021_175638 (2026-10-21): the settings had lived in Settings > Constellations (to 2026-10-19), then in the
Missions window's Controls tab; the user asked for a real place in the settings window with the graphics grouped,
sliders for the plumes ("I wanna test ranges"), and an engaging Kessler tracker (globe, danger zones, markers for the
last collisions). The plume / engine light / flame gains were choice rows (Off / x0.3 / x1 / x3) and rocket_gain had no
UI at all.
