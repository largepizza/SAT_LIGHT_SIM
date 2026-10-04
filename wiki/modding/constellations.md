# Constellations

`constellations.json` defines every satellite in the simulation. It has two lists: **satellite types**, which
say what a satellite is and how it reflects light, and **constellations** (orbital shells), which place many
copies of one type in orbit. This page documents every field of both, with examples and the usual mistakes.
The satellites' 3D geometry, which most types refer to, is on [Satellite models](satellite-models.md).

The file sits next to the executable and is read once at start-up. `constellations.schema.json` beside it is a
JSON Schema: point an editor at it (the shipped file already does, through `"$schema"`) for autocomplete,
tooltips and validation as you type.

## File structure

```json
{
  "$schema": "./constellations.schema.json",
  "version": 1,
  "satellite_types": [ ... ],
  "constellations": [ ... ]
}
```

| Key | Meaning |
|---|---|
| `$schema` | Editor support only; ignored by the program |
| `version` | Schema version, 1. Not read by the program |
| `satellite_types` | The list of types (below). Required by the schema |
| `constellations` | The list of shells (below). Required by the schema |

Missing optional keys take their defaults, and the program ignores keys it does not know (the schema flags them).
If the file is missing or is not valid JSON, the program uses a built-in roster instead and prints why on standard
error; see [Modding](index.md#failure-behaviour-at-a-glance).

## Satellite types

A constellation refers to a type by its `name`, exactly and case-sensitively. A type is described in one of two
ways:

- **A geometry model** (`"model"`): a separate file with the satellite's parts, materials and moving joints. Its
  brightness is a physical apparent magnitude computed from that geometry. Every type in the shipped roster but
  one is a model type.
- **The legacy two-surface form**: one or two reflecting surfaces with a Phong lobe each, an area, a diffuse floor
  and a mirror fraction. Brightness is in tuned display units rather than a physical magnitude, and features that
  need geometry (the 3D view, Go to, magnitude traces and exports, part shadowing) are not available.

### Model types

```json
{
  "name": "Starlink V2 Mini (Gen2)",
  "model": "starlink_v2_mini",
  "base_color": [0.80, 0.87, 1.00]
}
```

| Field | Type | Default | Meaning |
|---|---|---|---|
| `name` | string | required | Unique name; shells refer to it |
| `model` | string | none | Model id: loads `satellite_models/<id>.json` next to the executable |
| `base_color` | `[r, g, b]`, 0 to 1 | `[0.95, 0.95, 1.0]` | Tint of the satellite's point of light. Cosmetic; it does not change the brightness |

With `model` set, every legacy field (`cross_section_m2`, `primary`, `secondary`, `diffuse`, `mirror_frac`,
`attitude_groups`) is ignored: the model file owns the geometry and the attitude. If the model file cannot be
loaded, the type falls back to its legacy fields, so a model type with no legacy fields becomes a plain 10 m²
nadir-pointing satellite. The reason is in `satlight_log.txt`.

**Lobe budget.** At start-up each model is reduced to a list of reflection lobes (one per distinct surface
orientation and material). A type flown by more than 10 000 satellites in total gets at most 48 lobes, a type
flown by fewer gets up to 256, because the per-frame cost is the number of visible satellites times their lobes.
A complex model in a large shell is merged down to 48 lobes, which costs some magnitude accuracy. See
[Satellite photometry](../simulation/photometry.md).

**Mirror types.** A model whose root attitude group aims at `sun_reflect_ground_site` casts beams onto the ground.
The beam's mirror area and reflectance are taken from the model's faces that point along that group's primary
axis; with no such face the type casts no beams (and the log says so).

### Legacy types

```json
{
  "name": "My Legacy Sat",
  "base_color": [1.0, 0.9, 0.8],
  "cross_section_m2": 10.0,
  "primary":   { "attitude": "NadirPointing", "spec_exp": 18.0, "weight": 1.0 },
  "secondary": { "attitude": "SunTracking",   "spec_exp": 6.0,  "weight": 0.3 },
  "diffuse": 0.02,
  "mirror_frac": 0.05
}
```

| Field | Type | Default | Meaning |
|---|---|---|---|
| `name` | string | required | Unique name |
| `base_color` | `[r, g, b]`, 0 to 1 | `[0.95, 0.95, 1.0]` | Tint, multiplied into the flare. Required by the schema for a legacy type |
| `cross_section_m2` | number > 0 | 10 | Reflective area. Brightness scales with \( \sqrt{A / 10\,\text{m}^2} \). Required by the schema |
| `primary` | surface | nadir-pointing, Lambertian, weight 1 | The dominant surface. Required by the schema |
| `secondary` | surface | weight 0 (off) | An optional second surface |
| `attitude_groups` | list of groups | none | Explicit rigid attitude groups for the surfaces to mount on (below) |
| `diffuse` | 0 to 1 | 0.02 | A Lambertian floor, visible from every angle. 0 makes the satellite visible only while a surface is aligned |
| `mirror_frac` | 0 to 1 | 0 | Fraction of the primary that is a near-perfect mirror: adds a very narrow spike (300 times the Phong peak) on top of the lobe |

**Surfaces.** A surface is oriented either by a legacy attitude string or by mounting it on an attitude group:

```json
{ "attitude": "SunTracking", "spec_exp": 12.0, "weight": 0.3 }
{ "group": "wings", "normal": [0, 0, -1], "spec_exp": 18.0, "weight": 1.0 }
```

| Field | Type | Default | Meaning |
|---|---|---|---|
| `attitude` | string | `NadirPointing` | A legacy attitude mode (table below) |
| `group` | name or index | none | An entry of this type's `attitude_groups`. Wins over `attitude` when both are present |
| `normal` | `[x, y, z]` | `[0, 0, 1]` | The surface normal in the group's body frame (only with `group`) |
| `spec_exp` | number ≥ 0 | 0 | Phong exponent: 0 is Lambertian, about 18 a sharp panel flash, 200 a sub-degree spike |
| `weight` | number ≥ 0 | 0 | Contribution relative to the primary (the primary is normally 1) |

Mounting a surface on a group changes only where it points; the brightness model is still the legacy one.

### Legacy attitude modes

Each mode is converted at load into an attitude group plus a body normal. The program then only works with
groups, and the converted form is written to `satellite_types_resolved.json` in the user data folder.

| Mode | Surface normal | Typical use |
|---|---|---|
| `NadirPointing` | Toward the Earth's centre | Antenna face (Starlink) |
| `AntiNadir` | Away from the Earth's centre | Radiators facing space |
| `SunTracking` | Toward the Sun | Solar arrays |
| `SunTrackingTilted` | Toward the Sun, tilted toward the zenith by the *Flare mitigate tilt* setting | Arrays steered to keep their glint off the ground |
| `SunPerp` | Along the Sun × nadir direction: always edge-on to the Sun | Radiators that never see the Sun; visible through `diffuse` |
| `FlatMirror45` | Halfway between the Sun and nadir: reflects sunlight straight down | A mirror lighting the ground below it |
| `TargetedReflector` | Reflects sunlight onto the satellite's chosen ground site | Orbital mirrors; sites come from [`reflector_targets.json`](reflector-targets.md) |
| `KnifeEdge` | Nadir-pointing, rolled about the velocity axis to turn edge-on to the Sun, at most ±10° | Starlink's roll policy |
| `Tumbling` | Spins with the satellite's own random axis and rate | Debris |
| `Perpendicular` | Secondary surface only: +Y of the primary's group | Kept for old files; exact only when the primary is `SunTracking` or the weight is 0 |

An unknown mode string is reported on standard error and treated as `NadirPointing`.

### Attitude groups

An attitude group is a rigid body frame that some of the satellite holds still in. Its orientation is a
**two-vector law**: one body axis points exactly at a target, and a second body axis points as close as possible
at a second target, which fixes the roll. An optional one-axis **joint** then turns the group. The same format is
used by model files, which add parent-child trees; see
[Satellite models](satellite-models.md#attitude-groups-and-the-kinematic-tree) and [Attitude](../simulation/attitude.md).

```json
{
  "name": "wings",
  "primary":   { "axis": [0, 0, 1], "target": "nadir" },
  "secondary": { "axis": [1, 0, 0], "target": "velocity" },
  "joint": { "mode": "track", "axis": [0, 1, 0], "vector": [0, 0, -1], "target": "sun" }
}
```

| Field | Type | Default | Meaning |
|---|---|---|---|
| `name` | string | `group<index>` | Referred to by a surface's `group` |
| `law` | `two_vector` or `tumble` | `two_vector` | `tumble`: uncontrolled spin about the satellite's own random axis; body +Z is the spin axis and +X sweeps the plane around it |
| `primary.axis` | `[x, y, z]` | `[0, 0, 1]` | Body axis that points exactly at `primary.target` |
| `primary.target` | target | `nadir` | |
| `secondary.axis` | `[x, y, z]` | `[1, 0, 0]` | Body axis pointed as close as possible at `secondary.target`. Must not be parallel to the primary axis |
| `secondary.target` | target | `velocity` | |
| `joint.mode` | see below | `none` | Required inside `joint` |
| `joint.axis` | `[x, y, z]` | `[1, 0, 0]` | The rotation axis, in the group's body frame |
| `joint.vector` | `[x, y, z]` | `[0, 0, 1]` | The body vector that `track` and `edge_on` aim. Must not be parallel to the axis |
| `joint.target` | target | `sun` | What `track` and `edge_on` aim at |
| `joint.limit_deg` | number ≥ 0 | 180 | Rotation limit for `track` and `edge_on` |
| `joint.angle_deg` | number | 0 | The rotation for `fixed` |

Joint modes: `none`; `track` (turn so `vector` points as close as possible at `target`, like a solar-array
gimbal); `edge_on` (turn so `vector` is perpendicular to `target`, the smaller of the two solutions); `fixed` (a
constant `angle_deg`); `flare_mitigation_tilt` (the angle of the global *Flare mitigate tilt* setting).

Targets:

| Target | Direction |
|---|---|
| `nadir` / `zenith` | Toward / away from the Earth's centre |
| `sun` / `anti_sun` | Toward / away from the Sun |
| `velocity` / `anti_velocity` | Along / against the orbital velocity |
| `orbit_normal` / `anti_orbit_normal` | The orbit's angular-momentum axis, and its opposite |
| `sun_reflect_nadir` | The normal that reflects sunlight straight down |
| `sun_reflect_ground_site` | The normal that reflects sunlight onto this satellite's chosen ground site |

A legacy type's surfaces and groups together may use at most four groups (the schema allows two in
`attitude_groups`). Anything invalid (parallel axes, a joint vector along its axis, a surface naming a missing
group) is logged and the type falls back to a single nadir-pointing group.

## Constellations

Each entry is one shell: one type, at one altitude, in one pattern.

```json
{
  "name": "Starlink Gen2 shell 1",
  "type": "Starlink V2 Mini (Gen2)",
  "alt_km": 530.0,
  "incl_deg": 53.0,
  "num_planes": 72,
  "per_plane": 22,
  "enabled": true,
  "distribution": "Walker"
}
```

| Field | Type | Default | Meaning |
|---|---|---|---|
| `name` | string | required | Shown in the Constellations tab; also the key under which its on/off state is saved |
| `type` | string | required | A `satellite_types` name, exactly. An unknown name skips the shell |
| `alt_km` | number > 0 | required | Altitude above the Earth's mean radius (6371 km) |
| `num_planes` | integer ≥ 1 | required | Walker: number of planes. Other distributions: only a factor of the total |
| `per_plane` | integer ≥ 1 | required | Satellites per plane. **Total = `num_planes` × `per_plane`** for every distribution |
| `incl_deg` | 0 to 180 | 0 | Inclination. Above 90 is retrograde. RandomShell: the maximum |
| `enabled` | boolean | true | Whether the shell is shown (see the note on saved settings below) |
| `distribution` | `Walker`, `RandomShell`, `Disk` | `Walker` | How the orbits are generated. Any other value is treated as `Walker` |
| `alt_jitter_km` | number ≥ 0 | 0 | RandomShell and Disk: each satellite's altitude is offset by a uniform random amount within ±this |
| `raan_deg` | number | 0 | Disk: the plane's right ascension of the ascending node |
| `align_terminator` | boolean | false | Disk: put the plane on the day-night terminator, sun-synchronous (overrides `incl_deg` and `raan_deg`) |
| `num_rings` | integer ≥ 1 | 1 | Disk: number of concentric rings |
| `ring_spacing_km` | number ≥ 0 | 0 | Disk: altitude step between rings |

All orbits are circular.

### The three distributions

**Walker.** `num_planes` planes at inclination `incl_deg`, their ascending nodes evenly spread over 360°, each
with `per_plane` satellites. Each satellite's position along its orbit is random, so the planes are not phased
against each other.

**RandomShell.** `num_planes` × `per_plane` satellites, each with a random node, a random inclination between 0
and `incl_deg`, a random position along the orbit, and an altitude jittered by `alt_jitter_km`. Each also gets a
random tumble axis, rate and phase. Use it for debris and mixed-inclination populations.

**Disk.** Concentric rings in one orbital plane: the "flat disk" seen from orbit. The total `num_planes` ×
`per_plane` is divided evenly over `num_rings` rings (the last rings take any shortfall). The rings are centred on
`alt_km` and spaced by `ring_spacing_km`, so the shell spans `(num_rings − 1) × ring_spacing_km` of altitude.
Within a ring the satellites are evenly spaced. With `align_terminator: true`:

- each ring's inclination is the sun-synchronous inclination for that ring's own altitude (from the Earth's J2
  oblateness), so rings at different heights are tilted slightly differently;
- the node is set so that the plane lies along the terminator at the start of the simulation, and it then
  precesses one turn per year, keeping that relation to the Sun.

!!! warning "Tumbling types need RandomShell"
    Only RandomShell gives each satellite a tumble axis and rate. A type whose attitude is `tumble` (or the legacy
    `Tumbling`) flown in a Walker or Disk shell does not spin: every copy holds the same fixed orientation.

### Saved on/off state

The Constellations tab's ON/OFF and highlight switches are saved in `settings.json` under each shell's `name`, and
they win over `"enabled"` in this file at the next start. To make a change to `"enabled"` take effect, toggle the
shell in the tab, or give the shell a new name.

## Worked examples

### A new shell of an existing type

```json
{
  "name": "My polar shell",
  "type": "OneWeb Gen1",
  "alt_km": 1100.0,
  "incl_deg": 87.0,
  "num_planes": 36,
  "per_plane": 40,
  "distribution": "Walker"
}
```

Add it to `constellations`, restart, and it appears at the end of the Constellations tab with 1440 satellites.
The type name must match a `name` in `satellite_types` (check the shipped file for the exact spelling).

### A dawn-dusk mirror disk

The shipped Reflect Orbital shell: 5000 mirrors in three rings 10 km apart around 500 km, on the terminator.

```json
{
  "name": "Reflect Orbital",
  "type": "Reflect Orbital mirror",
  "alt_km": 500.0,
  "num_planes": 10,
  "per_plane": 500,
  "distribution": "Disk",
  "alt_jitter_km": 0.05,
  "align_terminator": true,
  "num_rings": 3,
  "ring_spacing_km": 10.0
}
```

### A debris cloud

```json
{
  "name": "Fragments",
  "type": "Debris fragment",
  "alt_km": 800.0,
  "incl_deg": 100.0,
  "num_planes": 1,
  "per_plane": 5000,
  "distribution": "RandomShell",
  "alt_jitter_km": 150.0
}
```

### The same satellite, legacy and explicit

`data/custom/constellations_attitude_example.json` defines a Starlink with the legacy `KnifeEdge` / `NadirPointing`
surfaces, and its exact twin written with explicit attitude groups:

```json
"attitude_groups": [
  { "name": "panel",
    "primary":   { "axis": [0, 0, 1], "target": "nadir" },
    "secondary": { "axis": [1, 0, 0], "target": "velocity" },
    "joint": { "mode": "edge_on", "axis": [1, 0, 0], "vector": [0, 0, 1], "target": "sun", "limit_deg": 10 } },
  { "name": "bus",
    "primary":   { "axis": [0, 0, 1], "target": "nadir" },
    "secondary": { "axis": [1, 0, 0], "target": "velocity" } }
],
"primary":   { "group": "panel", "normal": [0, 0, 1], "spec_exp": 18.0, "weight": 1.0 },
"secondary": { "group": "bus",   "normal": [0, 0, 1], "spec_exp": 5.0,  "weight": 0.2 }
```

The same file adds a type whose wing tracks the Sun on a gimbal, which no legacy mode can express. For a real
model type with parts and materials, start from a shipped model; see [Satellite models](satellite-models.md).

## The shipped roster

`data/constellations.json` carries 24 types (23 geometry models and one legacy comparison type) flown by 28 shells,
about 1.38 million satellites. All shells are enabled except the legacy comparison shell. The largest is the
orbital data-centre disk: one million satellites in 2000 rings 0.7 km apart around 1250 km.

## Limits

| Limit | Value | Notes |
|---|---|---|
| Satellites in all shells | 10 000 000 | Beyond this the roster is cut. GPU memory is about 100 bytes per satellite actually loaded |
| Shells with a button in the Constellations tab | 256 | Every shell loads and renders; only the first 256 can be toggled |
| Attitude groups | 4 per type | Parents before children in model files |
| Lobes per model type | 48, or 256 for types flown by at most 10 000 satellites | |

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| The roster looks like the built-in nine shells | `constellations.json` is not valid JSON; the parse error is on standard error |
| A shell never appears | Its `type` does not match a type `name` exactly, or it is switched off in the Constellations tab (the saved state wins over `enabled`) |
| A change to `enabled` has no effect | The saved setting for that shell name wins; toggle it in the tab |
| A model type is faint, featureless or has no 3D view | Its model failed to load and it fell back to legacy fields; read `satlight_log.txt` |
| Editing `cross_section_m2` or `primary` changes nothing | Correct for a model type: those fields are ignored. Edit the model file |
| A tumbling type does not tumble | It is in a Walker or Disk shell; use RandomShell |
| A mirror never casts beams | The model has no face along its site-aimed axis, or `reflector_targets.json` has no reachable site |
| An edit has no effect at all | The copy trap: see [Modding](index.md#how-changes-are-picked-up) |

## Where in the code

- `src/simulations/SatelliteSim.cpp`: `loadDefinitions()` (this file), `parseSurfaceSpec()`,
  `legacyAttitudeGroup()` and `resolveAttitude()` (legacy conversion), `buildOrbits()` (the distributions),
  `computeSSOInclination()`, `writeResolvedSatTypes()`, `loadHardcoded()` (the built-in roster).
- `src/simulations/SatModel.cpp`: `parseAttitudeGroupJson()`, `validateAttitudeGroups()`.
- `data/constellations.schema.json`: the schema.
