---
pages: [simulation/reflectors.md, modding/satellite-models.md, rendering/satellite-meshes.md, accuracy/provenance.md, accuracy/results.md, simulation/rockets.md]
code: [tools/make_reflector_model.py, tools/reflector_quad_analysis.py, data/satellite_models/reflect_orbital.json, src/simulations/SatModel.cpp, src/simulations/SatMesh.cpp, shaders/sat_mesh.vert, shaders/include/sat_mesh_common.glsl]
---
## Now
The Reflect Orbital mirror is the published quad-sail design: an 18 x 18 m square of four triangular aluminised-film
quadrants with scalloped edges (outer sag 0.9 m, boom edges 0.25 m) on two crossed telescoping booms along the
diagonals (26 m tip to tip), a 2.44 m bus behind it. 256.4 m2 of film (the 55 m plate it replaced had 3025 m2: the
mirror and its beams are 2.68 mag fainter). The quadrants are coplanar, so the photometry is one 256.4 m2 lobe.
At 500 km, normal incidence: a 4.2 km (half-peak) spot of 22.5 mW/m2 (2.1 lx, ~8x the full Moon), -14.8 mag from the
spot; a full 18 m square would be 0.25 mag brighter. Canting the quadrants by theta splits the spot into four spots
2 theta R out along the diagonals: the aim point stays lit below 0.13 deg (0.10 deg: -0.37 mag) and goes dark past it
(0.25 deg: -9.2 at the aim, four -13.3 spots 4.4 km out); a 2 cm billow spreads it to 8 km (-1.64 mag). Diffraction
(3e-8 rad) and the scallops' shape are invisible against the Sun's 9.3 mrad disc; the scallops matter only as area.
New model-format pieces: a `"type": "polygon"` primitive (`"vertices"` in local XY, counter-clockwise from +Z,
star-shaped; a membrane that never occludes) and a radial stow (`"stow": {"mode": "radial", "axis": [0,0,1]}`:
the extent across the axis shrinks, area as s^2). After release the booms telescope out and the film unfurls with
them, 10 min after release over 15 min. Generator knobs: `--side`, `--cant-deg`, `--outer-sag`, `--boom-sag`, `--gap`.

## History
2026-11-04: replaced the 55 x 55 m single plate (the project owner's quoted full-scale size, 2026-09-24) with the
published 18 m deployed configuration at the user's request. simulation/reflectors.md's "a 55 m square mirror at
600 km ... 2.8 km" example and rendering/satellite-meshes.md's "a 55 m Reflect" line describe the old plate.
