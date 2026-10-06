---
pages: [modding/satellite-models.md, rendering/points-and-meshes.md]
code: [shaders/sat_mesh.frag, shaders/include/sat_mesh_common.glsl, src/simulations/SatMeshRenderer.cpp]
---
## Now
A `"render_only": true` component is drawn by the mesh renderer but its light seeds neither the bloom
nor the mesh glare: its pixels carry a photometric share of 0. The bloom and glare of a meshed satellite
are normalised by the photometric model's intensity, which has no render-only parts in it.

## History
Until 2026-10-06 a sunlit aluminium fitting (render-only) on a Starmind satellite seeded white bloom
discs ~250 px wide while the model's own intensity toward the camera was ~0 (its arrays seen from
behind) — the user's snapshot from 968 km.
