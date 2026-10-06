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

Each entry is one constellation of one type. Most entries are one shell (one altitude, one pattern); a `Shells`
entry carries a whole filed table of shells (below).

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
| `alt_km` | number > 0 | required (not for Shells) | Altitude above the Earth's mean radius (6371 km) |
| `num_planes` | integer ≥ 1 | required (not for Shells) | Walker: number of planes. Other distributions: only a factor of the total |
| `per_plane` | integer ≥ 1 | required (not for Shells) | Satellites per plane. **Total = `num_planes` × `per_plane`** for Walker, RandomShell and Disk |
| `incl_deg` | 0 to 180 | 0 | Inclination. Above 90 is retrograde. RandomShell: the maximum |
| `enabled` | boolean | true | Whether the shell is shown (see the note on saved settings below) |
| `distribution` | `Walker`, `RandomShell`, `Disk`, `Shells` | `Walker` | How the orbits are generated. Any other value is treated as `Walker` |
| `alt_jitter_km` | number ≥ 0 | 0 | RandomShell and Disk: each satellite's altitude is offset by a uniform random amount within ±this |
| `raan_deg` | number | 0 | Disk: the plane's right ascension of the ascending node |
| `align_terminator` | boolean | false | Disk: put the plane on the day-night terminator, sun-synchronous (overrides `incl_deg` and `raan_deg`) |
| `num_rings` | integer ≥ 1 | 1 | Disk: number of concentric rings |
| `ring_spacing_km` | number ≥ 0 | 0 | Disk: altitude step between rings |
| `groups` | list of shell groups | none | Shells: the shell table (see [Shells](#shells)). Required for Shells |
| `cluster_size` | integer ≥ 1 | 1 | Walker, Disk and Shells: satellites per formation cluster. 1 = no clusters |
| `cluster_shape` | `line` or `ring` | `line` | How a cluster's members are arranged (see [Formation clusters](#formation-clusters)) |
| `cluster_spacing_km` | number > 0 | 1 | `line`: along-track distance between neighbouring members |
| `cluster_radius_km` | number > 0 | 0.2 | `ring`: radius of the polygon |
| `raan_spread_deg` | 0 to 90 | 0 | Shells, sun-synchronous groups only: each plane's node is offset from its terminator node by up to ±this |
| `raan_spread` | `even` or `random` | `even` | How the offsets of `raan_spread_deg` are chosen |

All orbits are circular.

### The distributions {#the-three-distributions}

**Walker.** `num_planes` planes at inclination `incl_deg`, their ascending nodes evenly spread over 360°, each
with `per_plane` satellites. Each satellite's position along its orbit is random, so the planes are not phased
against each other (with clusters, the clusters are evenly spaced from a random start instead).

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

**Shells.** A table of shell groups, such as the Table 1 of a filing with the US Federal Communications
Commission (FCC). Each group puts `shells` shells evenly over an altitude range, each with `planes_per_shell`
planes of `per_plane` satellites; a group is either sun-synchronous or a Walker set. See [Shells](#shells).

!!! warning "Tumbling types need RandomShell"
    Only RandomShell gives each satellite a tumble axis and rate. A type whose attitude is `tumble` (or the legacy
    `Tumbling`) flown in a Walker, Disk or Shells entry does not spin: every copy holds the same fixed orientation.

### Shells

A `Shells` entry has no `alt_km`, `num_planes`, `per_plane` or `incl_deg`; it has a `groups` list instead. Each
group is one row of a shell table:

| Field | Type | Default | Meaning |
|---|---|---|---|
| `alt_min_km` | number > 0 | required | Altitude of the lowest shell |
| `alt_max_km` | number > 0 | `alt_min_km` | Altitude of the highest shell |
| `shells` | integer ≥ 1 | required | Number of shells, spread evenly from `alt_min_km` to `alt_max_km` (a single shell sits at the middle of the range) |
| `planes_per_shell` | integer ≥ 1 | required | Orbital planes in each shell |
| `per_plane` | integer ≥ 0 | required | Satellites in each plane |
| `sun_synchronous` | boolean | false | Make every shell of the group sun-synchronous |
| `incl_deg` | 0 to 180 | 0 | Inclination of every shell (shorthand for equal minimum and maximum) |
| `incl_min_deg` / `incl_max_deg` | 0 to 180 | `incl_deg` | Inclination of the lowest / highest shell; the shells between run linearly from one to the other |

A group holds `shells` × `planes_per_shell` × `per_plane` satellites. Without clusters, the satellites of a plane
are evenly spaced around it from a random starting phase.

**Walker groups** (`sun_synchronous` false) spread their planes' nodes evenly over 360°, and stagger each shell
by a fraction 1/`shells` of the gap between planes, so the planes of neighbouring shells do not coincide.

**Sun-synchronous groups** ignore the inclination fields: each shell takes the sun-synchronous inclination for
its own altitude, and its node precesses once a year with the Sun, as a terminator-aligned Disk does. The planes
start at the **dusk node**, the node at 18:00 local time (LTAN 18:00), and are spread evenly in right ascension
from there. Two planes per shell therefore give the 18:00 and 06:00 planes: two near-polar rings along the
terminator that cross at the equator, an "X-ring" seen from the Sun.

**Node spread.** `raan_spread_deg` shifts each sun-synchronous plane's node by up to ± that many degrees from its
terminator node (15° is one hour of local time). With `raan_spread` set to `even`, the shifts follow a
golden-ratio sequence over the entry's planes, which covers the range evenly; `random` draws them uniformly,
which clumps. Without a spread, every shell of a group shares the same two planes, and shells only a couple of
kilometres apart, flying clusters a few kilometres across, pass through each other; see
[Orbits](../simulation/orbits.md#node-spread). Walker groups ignore the spread.

The derived values the rest of the program needs are filled in from the table: the total satellite count, and an
`alt_km` equal to the satellite-weighted mean altitude (where the 3D model view places the satellite). An entry
whose groups hold no satellites is skipped, with a line in `satlight_log.txt`.

!!! tip "Keep one filed table in one entry"
    The enable and highlight switches reach the GPU as two 32-bit masks, one bit per entry. An entry at position
    33 or later in the file has no bit of its own and cannot be switched on and off or highlighted reliably. A
    filing with dozens of shells therefore goes in one `Shells` entry rather than one entry per shell; the shipped
    roster has 29 entries.

### Formation clusters

With `cluster_size` above 1, the satellites of each plane (or each Disk ring) fly in clusters of that many
members, the clusters evenly spaced around the orbit. If the plane's count is not a multiple of `cluster_size`,
the last cluster is short.

- **`line`**: the members in a row along the track, `cluster_spacing_km` apart, centred on the cluster's
  position.
- **`ring`**: a regular polygon of `cluster_radius_km` radius in the orbit plane, spanned by the along-track and
  radial directions. Member *m* of *C* sits at angle 360° × *m* / *C* from the along-track direction toward the
  zenith, so the ring is as tall as it is long. Each member orbits at its own radius but at the cluster centre's
  angular rate, so the ring holds its shape: a station-kept formation, not a free one.

The clusters of a Walker or Shells plane start from a random phase; those of a Disk ring start at phase 0.

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

### A filed shell table in formation

A sun-synchronous X-ring and a set of Walker shells, flown in rings of eight satellites, 2 km in radius. The two
groups are the first group of each of the two shipped Starmind entries, combined here into one entry:

```json
{
  "name": "My orbital data centres",
  "type": "SpaceX Starmind AI1",
  "distribution": "Shells",
  "cluster_size": 8,
  "cluster_shape": "ring",
  "cluster_radius_km": 2.0,
  "raan_spread_deg": 10.0,
  "groups": [
    { "alt_min_km": 565.0, "alt_max_km": 585.0, "sun_synchronous": true,
      "shells": 10, "planes_per_shell": 2, "per_plane": 4168 },
    { "alt_min_km": 550.0, "alt_max_km": 568.0, "incl_min_deg": 26.0, "incl_max_deg": 32.0,
      "shells": 10, "planes_per_shell": 30, "per_plane": 272 }
  ]
}
```

The first group is ten sun-synchronous shells 2.2 km apart, each with an 18:00 and a 06:00 plane of 4168
satellites (521 rings), the planes' nodes spread over ±10°. The second is ten Walker shells of 30 planes, the
inclination rising from 26° in the lowest shell to 32° in the highest; `raan_spread_deg` does not touch them.
Together: 83 360 + 81 600 = 164 960 satellites. Keep `per_plane` a multiple of `cluster_size` so that no cluster
is short.

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

`data/constellations.json` carries 24 types (23 geometry models and one legacy comparison type) flown by 29
entries, about 1.37 million satellites. All entries are enabled except the legacy comparison shell. The largest
are SpaceX's Starmind orbital data centres: two `Shells` entries transcribed from SpaceX's filing with the FCC,
988 672 satellites in rings of eight (the sun-synchronous X-ring 499 072, the 30° shells 489 600); see
[Orbits](../simulation/orbits.md#the-starmind-roster) for how the table was derived.

## Limits

| Limit | Value | Notes |
|---|---|---|
| Satellites in all shells | 10 000 000 | Beyond this the roster is cut. GPU memory is about 100 bytes per satellite actually loaded |
| Entries with a button in the Constellations tab | 256 | Every entry loads and renders; only the first 256 have a button |
| Entries that can be switched and highlighted | 32 | The enable and highlight masks are 32 bits; put a large filed table in one `Shells` entry |
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
| A tumbling type does not tumble | It is in a Walker, Disk or Shells entry; use RandomShell |
| A `Shells` entry is missing, with a log line | Its groups hold no satellites (`per_plane` 0 or missing) |
| A shell's ON/OFF switch does nothing, or switches another one | The entry is 33rd or later in the file; merge shells into a `Shells` entry |
| Clusters in neighbouring shells pass through each other | Sun-synchronous shells share their planes; set `raan_spread_deg` |
| A mirror never casts beams | The model has no face along its site-aimed axis, or `reflector_targets.json` has no reachable site |
| An edit has no effect at all | The copy trap: see [Modding](index.md#how-changes-are-picked-up) |

## Where in the code

- `src/simulations/SatelliteSim.cpp`: `loadDefinitions()` (this file), `parseSurfaceSpec()`,
  `legacyAttitudeGroup()` and `resolveAttitude()` (legacy conversion), `buildOrbits()` (the distributions and the formation clusters),
  `computeSSOInclination()`, `writeResolvedSatTypes()`, `loadHardcoded()` (the built-in roster).
- `src/simulations/SatModel.cpp`: `parseAttitudeGroupJson()`, `validateAttitudeGroups()`.
- `data/constellations.schema.json`: the schema.
