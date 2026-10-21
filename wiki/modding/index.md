# Modding

SAT LIGHT SIM's satellites are data, not code. Every constellation, every satellite's, rocket's and station's
shape and materials, every launch vehicle and launch site, and every ground site an orbital mirror can aim at are
JSON files next to the executable, read at start-up. This section is the reference for those files: every field,
its default, what it does, and what goes wrong.

The how of what the data drives is described elsewhere: orbits on [Orbits and constellations](../simulation/orbits.md),
attitude on [Attitude](../simulation/attitude.md), brightness on [Satellite photometry](../simulation/photometry.md),
mirrors on [Reflectors and beams](../simulation/reflectors.md), launches on [Launches](../simulation/launches.md),
[Rockets](../simulation/rockets.md) and [Crew flights](../simulation/crew-flights.md).

## What can be modded

| File | Location (next to the executable) | What it defines | Page |
|---|---|---|---|
| `constellations.json` | `./` | Satellite **types** and the orbital **shells** that fly them | [Constellations](constellations.md) |
| `constellations.schema.json` | `./` | JSON Schema for the file above: editor autocomplete and validation | [Constellations](constellations.md) |
| `geo_satellites.json` | `./` | The named geostationary satellites of the GEO belt (a `List` entry's `members_file`) | [Constellations](constellations.md#named-satellites-list) |
| `satellite_models/<id>.json` | `satellite_models/` | A satellite's, station's, rocket's, pad's or ship's geometry: parts, materials, moving joints | [Satellite models](satellite-models.md) |
| `launches.json` | `./` | Launch sites, launch vehicles, the programs that keep constellations flying, crew and cargo flights, the deployment era | [Below](#the-launch-catalogue-launchesjson) |
| `launch_coasts.json` | `./` | Fine coastlines around coastal launch sites (generated) | [Below](#the-launch-catalogue-launchesjson) |
| `reflector_targets.json` | `./` | Ground sites that sunlight-reflecting mirrors aim at | [Reflector targets](reflector-targets.md) |
| `assets/sound/ambience/ambience.json` | `assets/sound/ambience/` | The ambient sound layers and what drives them | [Ambience](../sound/ambience.md) |
| `assets/sound/music/` | `assets/sound/music/` | The soundtrack: drop in MP3, FLAC or WAV files | [Music and tonality](../sound/music.md) |
| `<user data>/cinematics/*.json` | user data folder | Saved camera moves (written by the Cinematics window) | [Features](../using/features.md#cinematics) |

In the source tree the JSON files and `satellite_models/` live under `data/` (`data/constellations.json`,
`data/satellite_models/`, and so on). Several are written by scripts in `tools/` (the GEO belt, most models, the
launch coasts): edit the script, not its output; see
[Satellite models](satellite-models.md#generated-models). `data/custom/` holds worked examples and stress rosters that are not shipped:

| Example | What it shows |
|---|---|
| `constellations_attitude_example.json` | A legacy two-surface type, its explicit attitude-group twin, and a type with a gimbaled wing |
| `constellations_minimal.json` | A small roster, quick to load |
| `constellations_v1_1_default.json` | The previous default roster, all legacy types |
| `constellations_classic.json`, `constellations_2050.json`, `constellations_saturn_example.json` | Alternative rosters |
| `constellations_stress_10m.json`, `constellations_models_stress_10m.json` | Ten million satellites, for performance testing |

To try one, copy it over `constellations.json` next to the executable (keep a backup of the original).

## How changes are picked up

All of these files are read **once, at start-up**. There is no live reload: save, then restart the program.

!!! warning "The copy trap when building from source"
    A build copies `data/constellations.json`, `data/constellations.schema.json`, `data/reflector_targets.json`,
    `data/launches.json`, `data/launch_coasts.json`, `data/geo_satellites.json` and `data/satellite_models/` next
    to the executable whenever the source copies change. If you edit the copies
    next to the executable and then build after a source change, the build overwrites your edits. Pick one place
    to edit: the files in `data/` (and rebuild), or the files next to the executable (and do not build). In a
    release archive, the files next to the executable are the only copies.

**Settings override `enabled`.** Whether each constellation is shown is also saved in `settings.json`, by name. A
shell that you toggled in the Constellations tab keeps that state across restarts, whatever `"enabled"` says in
`constellations.json`. Rename the shell, or toggle it in the tab, to change it. A new name starts from the file's
value.

## The launch catalogue: launches.json

`launches.json` describes how the constellations are built up and kept flying. How the simulation uses it (the
schedule, trains, retirements, the deployment era) is on [Launches](../simulation/launches.md); rocket flight and
booster recovery on [Rockets](../simulation/rockets.md); crew and cargo flights on
[Crew flights](../simulation/crew-flights.md). The file's own `_comment` and `_deployment_comment` fields explain
the keys in place. A missing or invalid file switches launches off (logged as `launches: <reason>`); the
constellations still fly, all present at every date.

| Top-level key | What it holds |
|---|---|
| `sites` | Launch sites |
| `vehicles` | Launch vehicles and their flight figures |
| `programs` | Each keeps some constellations flying, or flies crew or cargo to stations |
| `station_arms` | Robotic arms on stations that berth cargo ships (Canadarm2 on the ISS) |
| `deployment_reference` | The date the programs' `present_2026` counts refer to |
| `min_first_life` | A first-filled slot's first refresh comes at least this share of the design life later (0.25) |
| `roster_history` | Constellations with no launches (stations, the 2020 benchmark shells, the debris field): when they are there |

**Sites** have an `id`, `name`, `lat`, `lon` and an `azimuth` range `[min, max]` (degrees clockwise from north,
allowed by range safety; may wrap through 360). Optional: `online` (the first date anything flies from it),
`online_for` (a later date per vehicle), `pad_kind` (0 generic, 1 Falcon-style service structure and landing zone,
2 Starship catch tower; by default from the vehicles there), `pad_heading_deg`, `pad_history`
(`[[date, kind], ...]`, kind −1 = nothing built: what the site's pad looks like over the years) and `fictional`.
Sites within 150 m share one pad.

**Vehicles** list the `sites` they fly from and their flight figures: staging (`meco_s`, `meco_alt_km`,
`meco_downrange_km`, `seco_s`), deployment (`deploy_after_seco_s`, `deploy_interval_s`, `sep_speed_mps`,
`sep_spread_mps`), engines (`flow1_kg_s`, `flow2_kg_s`, `exhaust1_m_s`, `exhaust2_m_s`, `flame1_mag_100km`,
`flame2_mag_100km`, `soot`), booster recovery (`booster`, `mount_m`, `catch_m`, `leg_m`, `droneship_km`,
`rtls_max_kg`, `landing_ships`), the payload's place on the stage (`fairing_sep_km`, `payload_base_m`,
`fairing_center_m`, `door_m`, `spacecraft_dock_m`), the upper stage (`upper_stage_deorbit`, `upper_stage_mass_kg`,
`vent_chance`) and `online`. A vehicle's id names its models: `rocket_<id>_stack`, `_booster`, `_upper` and so on in
`satellite_models/`.

**Programs** keep the constellations whose names contain one of their `match` strings flying, with the listed
`vehicles`: every orbital plane is refreshed once per `life_years`, in launches of `payload` satellites
`batch_gap_days` apart, deployed at `park_km`, raised over `raise_days`, while the satellites they replace deorbit
over `deorbit_days`. `sat_mass_kg` or `payload_mass_kg` picks the booster's recovery. Optional blocks:

| Block | Meaning |
|---|---|
| `deployment` | How the program's constellations are built up: `start`, `complete`, `present_2026`, `shell_stagger`, `steepness`, `early_vehicles` / `early_until`, `match` (one block per subset). See [Launches](../simulation/launches.md) |
| `stack` | How the satellites ride in the launcher: `layout` (`pez`, `flat` or `column`), `per_tier`, `side_axis`, `pitch_m`, `offset_m`, `base_m`, `column_radius_m`, `member_center`, `dispenser`, and per-program `deploy_interval_s` / `sep_speed_mps` / `sep_spread_mps`. Without it the satellites appear stowed at the dispenser point |
| `crew` | A crew or cargo flight to a station (a constellation of one): `spacecraft`, `people`, `model`, `heat_shield_m`, `lands`, `fast_dock_h`, `cargo`, `return`, `ports` (a port class matched against the station model's port `use`), `share_crew_ports`, `stay_days`, and for a vehicle the station's arm captures, `berth`, `grapple` ([radius, depth below the port] of its grapple fixture) and `capture_m`. `life_years` is then a fixed cadence, not scaled by the design-life setting, and `raise_days` is lift-off to docking. See [Crew flights](../simulation/crew-flights.md) |

**Station arms** each match one station (`match`, constellation-name substrings) and describe an arm in the
station's body frame (+X along the velocity, +Z to nadir), all lengths in metres:

| Field | Default | Meaning |
|---|---|---|
| `model` | `canadarm2` | Its meshes: `rocket_arm_<model>_boom.json` (drawn twice) and `rocket_arm_<model>_wrist.json` |
| `base`, `base_normal` | `[0, 0, 0]`, `[0, 0, 1]` | Where the arm is mounted and the direction its shoulder housing stands out |
| `shoulder_m`, `boom_m`, `wrist_m` | 1.4, 7.1, 1.6 | The shoulder housing, each of the two equal booms, the wrist and end effector |
| `park_wrist`, `park_dir` | `[0, 0, 0]`, `[0, 0, 1]` | The parked wrist point and the direction the end effector points from it |
| `pole` | `[0, 1, 0]` | The side the elbow bends toward |

A berthing vehicle's capture and release points must lie within the arm's reach (two booms from the shoulder, plus
the wrist); `launch check` reports any shortfall as `berth_arm_reach_short_m`. See
[Crew flights](../simulation/crew-flights.md#berthing).

!!! warning "The stack numbers follow the models"
    `tools/make_rocket_models.py` measures each satellite model stowed (its `stow` and `fold`), derives the stack
    numbers, checks that the stack fits every fairing the program flies in, and writes the dispensers. After
    changing a satellite model's stowed shape or a vehicle's fairing, re-run it, and run it with `--check` to see
    whether `launches.json` still agrees.

`launch_coasts.json` is written by `tools/make_pad_coasts.py` from the coastal sites in `launches.json`; re-run it
after adding or moving a coastal site. Without it the pads stand on the global water map. See
[Launch visuals](../rendering/launch-visuals.md).

## Validation and logs

Problems are reported in two places:

- **`satlight_log.txt`** in the user data folder (see [Getting started](../using/getting-started.md#where-files-live)).
  Every satellite model is logged as it loads and bakes, with its group, part and triangle counts, its lobe
  count and budget, and the error of its baked brightness against a brute-force check:

    ```
    model '<id>' (type '<type name>', <n> satellites): <g> groups, <c> components, <t> triangles
        -> <e> exact lobes -> <l> lobes (budget <b>); validator |dmag| p95 <x>, max <y>
    ```

    Model warnings (an unknown preset, pattern or distribution, a bad coverage value) and model load failures
    are logged here, as are attitude-group problems in `constellations.json` types.
- **Standard error** (the console window on Windows, the terminal elsewhere). File-level problems go here and
  **not** into the log: a missing or unparsable `constellations.json` or `reflector_targets.json`, a shell whose
  `type` names no satellite type, an unknown legacy attitude string, an unknown reflector target `kind`, too
  many targets, and the final "Loaded N satellite type(s) and M constellation(s)" summary.

Two generated files help you see what the loader made of your data. Both are written to the user data folder at
every launch, and neither is ever read back, so deleting them is always safe.

| File | Contents |
|---|---|
| `satellite_types_resolved.json` | Every loaded satellite type after conversion. A legacy type is written out with explicit attitude groups; a model type is written as its `name`, `model` and `base_color`. Entries paste straight back into `constellations.json` |
| `satellite_models_debug/<id>_rest.obj`, `<id>_sunlit.obj` | Each model's shape (with `.mtl` materials) in its rest pose and in a sunlit pose, Y up with zenith up. Open in any 3D viewer to check the geometry |

For model work there is also a command-line tool, **SatModelTool**, which loads, bakes, validates and exports a
model without starting the program. See [Satellite models](satellite-models.md#validating-a-model-satmodeltool).

## Failure behaviour at a glance

| What fails | What happens |
|---|---|
| `constellations.json` missing or not valid JSON | A built-in roster (eight legacy types flown by nine shells) is used instead |
| A shell names an unknown type | That shell is skipped |
| A model file is missing or invalid | The type falls back to its legacy fields (`cross_section_m2`, `primary`, ...), or to a plain 10 m² nadir-pointing type if it has none |
| A legacy type's attitude groups are invalid | The type falls back to one nadir-pointing group |
| `reflector_targets.json` missing, invalid or empty | About 200 randomly placed ground sites are generated |
| `launches.json` missing or invalid | No launches; the constellations are all there at every date |
| A `List` entry's `members_file` missing or invalid | Logged; only the inline members fly (an entry with none is skipped) |
| More than 201 reflector targets | The list is cut at 201 |
| More than 10 000 000 satellites in total | The roster is cut at 10 000 000 |

## Where in the code

- `src/simulations/SatelliteSim.cpp`: `loadDefinitions()` (constellations), `loadHardcoded()` (the built-in
  roster), `loadModelType()` and `bakeModelType()`, `writeResolvedSatTypes()`, `buildOrbits()`,
  `loadReflectorTargets()`.
- `src/simulations/Launches.cpp`: the `launches.json` loader (`launch::Catalog::load()`);
  `src/simulations/SatelliteSimLaunches.cpp`: everything that flies from it.
- `src/simulations/SatModel.cpp`: `loadSatModel()`, `parseAttitudeGroupJson()`, `satMaterialPresets()`.
- `src/Paths.cpp`: the executable and user data folders.
- `tools/sat_model_tool/`: SatModelTool.
