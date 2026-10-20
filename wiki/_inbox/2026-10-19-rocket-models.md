---
pages: [modding/satellite-models.md, simulation/launches.md, rendering/meshes.md]
code: [src/simulations/SatModel.h, src/simulations/SatModel.cpp, src/simulations/SatMesh.cpp, shaders/sat_mesh.frag, shaders/sat_orbit.comp, tools/make_rocket_models.py, src/simulations/SatelliteSimLaunches.cpp]
---
## Now
**Two new primitives in satellite model files.**
- `"type": "profile"` — a body of revolution about local Z from `"profile": [[z, r], ...]` (bottom to top along the
  outside). Ogive noses, domes, fairings, boattails and engine bells. Options: `"caps"` (flat end discs), an
  `"arc_material"` over `"arc_deg": [start, end]` (degrees of azimuth from local +X toward +Y), and
  `"inner_material"` (the inside drawn too; render mesh only). Its shadow is the exact cone frustum through its end
  radii.
- `"type": "fin"` — a tapered, swept, thick fin: `root_chord`, `tip_chord`, `span` (local +X), `sweep` (tip leading
  edge aft of the root's), `thickness` / `tip_thickness` (local Y), `bevel` (edges thin to a knife edge over that
  share of the chord). `back_material` paints the -Y faces. Its shadow is its bounding box.

**`hex_tiles` material pattern.** Heat-shield tiles: 0.31 m hexagons with thin off-white gaps and a per-tile shade
(and rare pale replacement tiles). Photometrically neutral like the other patterns; fades out below ~3 px a tile.

**Launch vehicle models reworked against references** (issue 20261019_181320). All 16 vehicles are rebuilt from
public dimensions (cited in each model's `sources`): Starship ship with an ogive nose, the tiled windward half,
tapered flaps with thickness, 3 sea-level + 3 vacuum Raptors; Super Heavy (72.3 m) with the hot-staging ring, three
grid fins and 33 Raptors in 3/10/20 rings; Falcon 9 with the octaweb's 9 engines, MVac, legs and grid fins (stowed
on the stack, deployed on the returning booster); Crew Dragon with its solar-cell trunk; Soyuz's tapered strap-ons;
escape towers on the crewed vehicles. Every engine is its own bell, and the flame starts at the bells' exit plane
and spans the cluster.

## History
Until 2026-10-19 rockets were cylinders and straight cones with flaps as flat planes, the heat shield as five flat
black panels, and one cone standing for all the engines (seen from behind: a flat disc).
