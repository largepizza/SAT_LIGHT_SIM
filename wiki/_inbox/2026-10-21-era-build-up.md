---
pages: [simulation/launches.md, modding/satellite-models.md, modding/index.md, sound/index.md, using/settings.md, development/harness.md]
code: [src/simulations/SatelliteSimLaunches.cpp, src/simulations/Launches.cpp, data/launches.json, src/simulations/SatModel.cpp, shaders/sat_orbit.comp, shaders/sat_mesh.vert, shaders/include/sat_mesh_common.glsl, tools/make_station_models.py, src/simulations/SatelliteSimAmbience.cpp, src/simulations/SatelliteSimUI.cpp]
---
## Now
**Pads by date.** A launch site's pad has a history: `pad_history` [[date, kind], ...] in data/launches.json (kind 0
generic, 1 Falcon, 2 Starship, -1 nothing), else a generic pad from its `online` date raised to each vehicle's kind from
its `online_for` date there. The mesh, ground pattern, graded terrain, city-light exclusion and floodlights show the pad
as it stood: 39A a Shuttle-era pad, Falcon from 2017, its Starship tower from 2027-03; SLC-37 a Delta IV pad until 2025,
then Starship from 2027-09; Louisiana and the sea platforms nothing before their dates. Ships are caught only at pads
with a tower by then.

**Stations built module by module.** A model component (and docking port) may carry `"since": "YYYY-MM-DD"`; with the
deployment era on it is neither drawn, nor shadowing, nor counted in the brightness before that date (lobes never merge
across dates). Tiangong: Tianhe 2021-04, Wentian 2022-07, Mengtian 2022-10; Axiom, Haven-2, ROS, BAS and Orbital Reef
follow the operators' sequences (scenario dates after 2026). Crews dock only at ports already there.

**Benchmark shells.** The 2020 V1.0 / VisorSat shells (`"benchmark": true` in roster_history) are left out of the
era's sky by default: Starlink Gen1's own first fills stand for the same satellites. Settings > Constellations >
BENCHMARK SHELLS (2020) "In the deployment era" (`launches.era_benchmark_shells`, default off) brings them back.

**Ambience.** The satellite-shell sound counts only satellites there now (each constellation's / shell group's deployed
share), so a 2020 sky hums with Gen1's launches, not the 2036 shells.

**Constellations tab.** Unfolded groups, the open member list and its search are saved (`constellations_ui`).

## History
Until 2026-10-21 pads had one fixed kind (39A showed its Starship tower in 2019), stations appeared whole on their first
module's date, ~1.5k Starlinks were counted twice in 2019-2024, and the ambience heard the 2036 roster at every date.
OneWeb / Amazon Leo stacks were rechecked: with the models' stowed sizes (~2x the real satellites) no column fits their
fairings (OneWeb misses Falcon 9 by 0.08 m at best); left unstacked.
