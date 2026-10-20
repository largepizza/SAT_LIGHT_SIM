---
pages: [simulation/launches.md, modding/satellite-models.md, development/harness.md]
code: [src/simulations/SatelliteSimLaunches.cpp, src/simulations/Launches.cpp, src/simulations/SatModel.cpp, shaders/sat_orbit.comp, shaders/sat_mesh.vert, data/satellite_models/spacex_ai_sat.json, data/launches.json]
---
## Now
A train member leaves its dispenser stowed and with the dispenser's own velocity plus a small push; from there it flies
free (Clohessy-Wiltshire motion about the parking orbit, its offset from its own path eased out over two days).
Starship releases one satellite every 45 s at 0.7 m/s (2.5 cm/s spread), so a Starmind train leaves as one clean string
along one arc, about 50 m apart, curving up and back from the ship.

Satellites unfold after release. A model's components may be stowed (`"stow": {"axis", "root", "scale", "phase"}`:
packed toward the plane dot(axis, x) = root to `scale` of their extent, an accordion array or a folded radiator) and the
model gives the timeline (`"deployment": {"unfold_delay_s", "unfold_s"}`, +-15% per member). Starmind: radiators
unfold first, then the trusses and wings, starting ~10 min after release and lasting ~10 min; Starlink V2 Mini / DTC /
V3: the arrays, ~15 min after release, over 4 min (estimates). Then the train flies "open book" until it reaches its
slot. The photometry follows the unfolding (each lobe's area scales with its stowed parts), on the GPU and the CPU.

Train records are the orbit the member is really on (its own velocity, the SSO node rate, the float mean motion the GPU
multiplies), so the satellites no longer jump at each rewrite: a few cm on the CPU path (meshes), ~1 m on the GPU
(its float floor) instead of tens to hundreds of metres. Harness: `launch jitter`, `launch legacy on|off`,
`launch deploy filter=Starmind`, `launch select kind=train member=0`; scripts `starmind_deploy.satcmd`,
`deploy_jitter.satcmd`.

## History
2026-10-19: issue 20261019_205535 ("serious jitter on sat positions at 10x during deployment"). The records had been
built from the bare circle's velocity while the member drifted in a straight line in the rotating frame and kept a
fixed dispenser offset of up to ~80 km across (a constant 0.07 m/s^2 push no orbit has), had no SSO node rate, and
used the double mean motion where the GPU multiplies its float: jumps up to ~2.4 px a frame at 1 km at 10x. Before
this the members were drawn in the open-book pose from the moment they left the dispenser, so a Starmind's 70-m wings
overlapped the ship.
