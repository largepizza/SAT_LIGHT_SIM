---
pages: [using/missions.md, using/interface.md, rendering/meshes.md, rendering/sky.md, using/graphics-settings.md]
code: [src/simulations/SatelliteSim.cpp, src/simulations/SatelliteSimUI.cpp, src/simulations/SatelliteSimMissions.cpp, src/simulations/SatMeshRenderer.cpp, shaders/sat_sky.frag, shaders/sat_mesh_marker.frag, shaders/include/sat_mesh_common.glsl]
---
## Now
- The Info window's MISSION progress bar is progress through the milestone list, interpolated in time between the
  last milestone passed and the next one, so the bar and "(n of N milestones)" agree.
- The selection panel's magnitude follows the Info window's MAGNITUDE tile: what the GROUND observer sees ("below
  your horizon", "dark", or the magnitude after extinction), also in follow mode (it was the camera's view, and a
  spacecraft in the Earth's shadow read "Mag 23.3").
- The Info window's 3D view draws a burning stage's flame (from the model's engines along its axis, longer and wider
  in thin air) and its glowing nozzles, and frames the flame. A destroyed or re-entered roster object shows no model,
  only "Destroyed <date>" / "Re-entered <date>" over the sky where it was.
- Missions > Ongoing lists crew transfers, stations, rockets and trains first; the deployment-era BUILD-OUT summary
  is last, collapsed ("n of N constellations complete", Show / Hide) once every cohort is complete.
- Window titles too long for the title bar are cut with "..." before the title-bar icons.
- The Sun's lens-flare ghost discs fall off as the cube of their old Lorentzian: they no longer lay a brown wash
  over half the frame when the Sun is in view from orbit (a backlit station 150 m away vanished under it). The Sun's
  glare halo is kept over a satellite mesh while the Sun itself is visible (veiling light).
- Planetarium (SKY_LITE) draws the green airglow layer in closed form: the faint green limb line that shows the
  Earth's horizon at night from orbit, and its glow low in the sky from the ground (+3 KB of SPIR-V).

## History
2026-10-21 QA sweep: Haven-2 backlit from 158 m showed only edge highlights. Its earthlit side was drawn (~0.003
linear, visible at +3 EV); the lens flare's ghost tails (~0.05 linear) covered it. Bloom, glare sprites, satellite
adaptation and auto exposure were ruled out by toggling each.
