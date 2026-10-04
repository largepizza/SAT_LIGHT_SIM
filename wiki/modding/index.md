# Modding

SAT LIGHT SIM's satellites are data, not code. Every constellation, every satellite's shape and materials, and
every ground site an orbital mirror can aim at are JSON files next to the executable, read at start-up. This
section is the reference for those files: every field, its default, what it does, and what goes wrong.

The how of what the data drives is described elsewhere: orbits on [Orbits and constellations](../simulation/orbits.md),
attitude on [Attitude](../simulation/attitude.md), brightness on [Satellite photometry](../simulation/photometry.md),
mirrors on [Reflectors and beams](../simulation/reflectors.md).

## What can be modded

| File | Location (next to the executable) | What it defines | Page |
|---|---|---|---|
| `constellations.json` | `./` | Satellite **types** and the orbital **shells** that fly them | [Constellations](constellations.md) |
| `constellations.schema.json` | `./` | JSON Schema for the file above: editor autocomplete and validation | [Constellations](constellations.md) |
| `satellite_models/<id>.json` | `satellite_models/` | A satellite's geometry: parts, materials, moving joints | [Satellite models](satellite-models.md) |
| `reflector_targets.json` | `./` | Ground sites that sunlight-reflecting mirrors aim at | [Reflector targets](reflector-targets.md) |
| `assets/sound/ambience/ambience.json` | `assets/sound/ambience/` | The ambient sound layers and what drives them | [Ambience](../sound/ambience.md) |
| `assets/sound/music/` | `assets/sound/music/` | The soundtrack: drop in MP3, FLAC or WAV files | [Music and tonality](../sound/music.md) |
| `<user data>/cinematics/*.json` | user data folder | Saved camera moves (written by the Cinematics window) | [Features](../using/features.md#cinematics) |

In the source tree the first four live under `data/` (`data/constellations.json`, `data/satellite_models/`, and
so on). `data/custom/` holds worked examples and stress rosters that are not shipped:

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
    A build copies `data/constellations.json`, `data/constellations.schema.json`, `data/reflector_targets.json`
    and `data/satellite_models/` next to the executable whenever the source copies change. If you edit the copies
    next to the executable and then build after a source change, the build overwrites your edits. Pick one place
    to edit: the files in `data/` (and rebuild), or the files next to the executable (and do not build). In a
    release archive, the files next to the executable are the only copies.

**Settings override `enabled`.** Whether each constellation is shown is also saved in `settings.json`, by name. A
shell that you toggled in the Constellations tab keeps that state across restarts, whatever `"enabled"` says in
`constellations.json`. Rename the shell, or toggle it in the tab, to change it. A new name starts from the file's
value.

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
| More than 201 reflector targets | The list is cut at 201 |
| More than 10 000 000 satellites in total | The roster is cut at 10 000 000 |

## Where in the code

- `src/simulations/SatelliteSim.cpp`: `loadDefinitions()` (constellations), `loadHardcoded()` (the built-in
  roster), `loadModelType()` and `bakeModelType()`, `writeResolvedSatTypes()`, `buildOrbits()`,
  `loadReflectorTargets()`.
- `src/simulations/SatModel.cpp`: `loadSatModel()`, `parseAttitudeGroupJson()`, `satMaterialPresets()`.
- `src/Paths.cpp`: the executable and user data folders.
- `tools/sat_model_tool/`: SatModelTool.
