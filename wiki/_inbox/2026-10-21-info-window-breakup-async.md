---
pages: [using/features.md, using/missions.md, simulation/kessler.md, simulation/launches.md, rendering/satellite-meshes.md, rendering/aurora-airglow.md, rendering/hardware-tiers.md]
code: [src/simulations/SatelliteSimUI.cpp, src/simulations/SatelliteSimLaunches.cpp, src/simulations/SatelliteSimKessler.cpp, src/simulations/SatelliteSim.cpp, src/simulations/SatMeshRenderer.cpp, shaders/sat_mesh.frag, shaders/sat_sky.frag]
---
## Now
- The Info window's STATUS time row reads long durations as days / hours / minutes, and a visiting spacecraft's by
  phase: "Since separation", "Docked since <UTC> (n days)" (or "Berthed since"), "Since undocking" /
  "Since unberthing", "Since entry interface".
- Exploding a satellite before the Sun's Fury no longer freezes the frame: the debris world is rebuilt on a
  background thread while the clock holds briefly under the banner "Computing the debris cascade: the breakup"
  (about 0.15 s); the result is the same.
- At 1 month/s - 1 year/s the launch schedule's pad deconfliction builds each day's rival list in parallel: the
  worst frames carry about half the launch work they did.
- A burning engine lights its rocket's hull at night in the Info window's 3D view (and close up in the main view):
  the engine's light is treated as a sphere, so the hull above it, at a grazing angle, is lit.
- The Info window's 3D view of the ISS shows Canadarm2 in the pose the main view draws.
- Planetarium from orbit at night: the closed-form green airglow is a line at the limb only; the night disc is no
  longer tinted green.

## History
2026-10-21: a docked Progress read "Since separation 10086696 s"; a pre-CME Explode cost ~220 ms on the main thread
plus ~75 ms of launch-plane rebuild the next frame (the click frame is now ~13 ms of that); a single rival day
bucket at 1 yr/s took ~40 ms on one thread.
