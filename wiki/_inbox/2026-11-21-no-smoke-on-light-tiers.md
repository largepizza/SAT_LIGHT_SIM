---
pages: [simulation/wildfires.md, rendering/hardware-tiers.md]
code: [src/simulations/SatelliteSimWildfire.cpp, src/simulations/SatelliteSimSmokeLayer.cpp, src/simulations/SatelliteSim.h]
---
## Now
Wildfire smoke (the streaks, the smoke at the flames, the palls and the smoke layer) is built and drawn on the full
graphics tiers only. On Planetarium and Potato (`wildfireSmokeDrawn()` false) the wildfire fill treats the smoke gains
as 0: the dust list holds no smoke Gaussians, the smoke layer's map is empty and its pass is not recorded. The fires
themselves (front points, flames, embers, their light) are drawn as on the other tiers. The user's "Smoke" setting is
not changed by the preset.

## History
Until 2026-11-21 Planetarium drew the smoke: over its flat ground, from orbit, the layer read as a grey haze with an
orange-brown wedge and a hard straight edge (issue_20261121_160405, shot 8). Potato already skipped the layer's pass
but still built the smoke on the CPU.
