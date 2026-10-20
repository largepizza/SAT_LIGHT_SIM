---
pages: [rendering/satellite-meshes.md, development/profiling.md]
code: [shaders/sat_mesh.frag, src/simulations/SatMeshRenderer.cpp, src/simulations/SatelliteSimUI.cpp]
---
## Now
A satellite mesh's self-shadows (sun, light through translucent blankets, and reflections blocked by
other parts) are exact ray tests against the model's primitives, accelerated per instance: once a frame
the CPU poses every blocking primitive in world space with a bounding sphere and bins their shadows on a
16x16 grid across the Sun, so a pixel's sun ray tests only the few primitives whose shadow can reach it,
and every ray first rejects a primitive by its sphere. The 64 instances largest on screen, the info
window's 3D view and the photometric check use it. Close-up stations now cost about 1 ms of mesh
rendering at 1920x1009 (the ISS from 12-60 m was 59-69 ms), and the cost no longer grows with
render-only detail parts. Knockout `mesh_shadow_accel_disabled` (bit 8388608, "Mesh shadow accel OFF
(A/B)") restores the old per-pixel loop for comparison; the image is the same.

## History
Until 2026-10-20 each shadow ray looped over every primitive of the model and recomputed its pose per
pixel; a user report (issue 20261019_225545, shot 3) measured 85 ms GPU frames next to the ISS.
Before -> after (mesh pass, RTX 3070 Ti): ISS report pose 68.9 -> 1.1 ms, 50 m 58.5 -> 0.9, 200 m 6.3 -> 0.4,
1 km 0.6 -> 0.3; Tiangong 40 m 5.9 -> 0.3; Orbital Reef 60 m 3.9 -> 0.3.
