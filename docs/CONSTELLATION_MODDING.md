# Constellation Modding Guide

`constellations.json` defines every satellite in the simulation: the **satellite types** (what a
satellite is, and how it reflects light) and the **constellations** (the orbital shells that
instantiate them many times over). Modding the roster starts there.

A type can either be described by its **geometry** — `satellite_models/<id>.json`, the route every
shipped type now takes — or by the **legacy two-surface** fields that predate it. Both are documented
below, because both still load and a mod may mix them freely.

Both files sit next to the built executable and are read once at startup. If a file is missing, or
contains a JSON syntax error, the simulation falls back to its built-in defaults and carries on.

The machine-readable field reference is `constellations.schema.json`, which ships beside
`constellations.json` and gives VS Code autocomplete and validation. **This guide explains intent; the
schema is authoritative about what is allowed.**

---

## Where the files live

| File | Location | Notes |
|---|---|---|
| `constellations.json` | next to the executable | source: `data/constellations.json`, copied by the build |
| `constellations.schema.json` | next to the executable | source: `data/`, autocomplete + validation for the file above |
| `satellite_models/<id>.json` | `<exe>/satellite_models/` | source: `data/satellite_models/`; loaded by a type's `"model": "<id>"` |
| `reflector_targets.json` | next to the executable | ground sites that a `sun_reflect_ground_site` mirror may aim at |
| `satellite_types_resolved.json` | **user data folder** | written on every launch: each loaded type in explicit form |
| `data/custom/*.json` | source tree only | worked examples and stress rosters; not shipped |

The user data folder is `%APPDATA%\SatLightSim` (Windows), `~/Library/Application Support/SatLightSim`
(macOS), `$XDG_DATA_HOME/SatLightSim` or `~/.local/share/SatLightSim` (Linux), or the exe folder if
none of those is writable.

`satellite_types_resolved.json` is the one to know about. Every type is normalised at load — legacy
attitude strings become explicit attitude groups, preset materials are written out field by field —
and this file *is* that normalised form. **Paste an entry from it back into `constellations.json` and
it renders identically**, which makes it the reliable way to see what the loader made of your file. It
is only ever written, never read, so deleting it is always safe.

---

## Quick start

1. Open `constellations.json` (next to the exe) in VS Code — autocomplete comes from the bundled schema.
2. To restyle an existing satellite, edit the model in `satellite_models/<id>.json` that its type
   names. To add a population, add a type and then a constellation shell that references it by name.
3. Save and restart — both files are read once at startup.
4. If a shell does not appear, look for `[SatelliteSim]` warnings: a `type` that does not match a
   `satellite_types` name exactly is the usual cause.
5. Keep changes small and restart often. There is no live reload, and one malformed entry can cost you
   the whole file's customisations (the built-in defaults then take over).

---

## File structure

```jsonc
{
  "$schema": "./constellations.schema.json",  // editor autocomplete + validation
  "version": 1,
  "satellite_types": [ /* what a satellite is: geometry, or legacy surfaces */ ],
  "constellations": [ /* orbital shells: how many, how high, in what pattern */ ]
}
```

Unknown keys are ignored by the loader (the schema will flag them); missing keys silently take their
default, so a file written for an older build keeps loading.

---

## Satellite types

Each entry in `satellite_types` is referenced by name from a constellation's `"type"`. Naming is
exact-match and case-sensitive. There are two mutually exclusive ways to describe one.

### Route A — a geometry model (what ships)

```jsonc
{
  "name": "My Satellite",
  "model": "my_satellite",          // loads <exe>/satellite_models/my_satellite.json
  "base_color": [1.0, 0.95, 0.9]    // optional; sprite tint only (default near-white)
}
```

- The model file describes primitives, materials and a kinematic tree of attitude groups. Brightness
  becomes a **physical apparent magnitude** derived from that geometry — no `cross_section_m2`, no
  `spec_exp`, no `mirror_frac`.
- When `model` is present the legacy brightness fields are **ignored** (`base_color` is the exception:
  it stays the point-sprite tint, so a type still looks right before it resolves into a mesh).
- If the model file cannot be loaded, the loader warns and falls back to the type's legacy fields —
  which means a model type should either ship sensible legacy fields or accept being invisible.
- A model type may set `"attitude_groups"` too, but it has no effect: the **model file owns the
  attitude tree** for model types.

### Route B — the legacy two-surface form

```jsonc
{
  "name": "My Legacy Sat",
  "base_color": [1.0, 0.9, 0.8],  // RGB tint, linear [0–1]; multiplies the computed flare
  "cross_section_m2": 10.0,       // total reflective area; brightness ∝ sqrt(area / 10)
  "primary":   { "attitude": "NadirPointing", "spec_exp": 18.0, "weight": 1.0 },
  "secondary": { "attitude": "SunTracking",   "spec_exp": 6.0,  "weight": 0.3 },
  "diffuse": 0.02,                // isotropic floor [0–1]: visible from every angle
  "mirror_frac": 0.05             // near-perfect mirror fraction [0–1]; 0 disables
}
```

| Field | Meaning |
|---|---|
| `base_color` | RGB tint, linear, 0–1. Cosmetic (it multiplies the flare), not photometric. |
| `cross_section_m2` | Reflective area in m². Reference points: Starlink ≈ 10, ISS ≈ 250, a full-scale Reflect Orbital mirror ≈ 2376. |
| `primary` | The dominant surface. Required. |
| `secondary` | Second surface. Optional; `weight: 0` disables it. |
| `diffuse` | Lambertian floor visible at any angle. `0` = the satellite is only ever visible while specularly aligned. |
| `mirror_frac` | Fraction of the primary that behaves as a near-perfect mirror; adds an ultra-narrow spike (`MIRROR_BOOST = 300`) on top of the Phong lobe. Keep at 0 for anything that is not actually a mirror. |

A surface is either aimed by a legacy attitude string **or** mounted on an attitude group — the schema
requires one of the two:

```jsonc
{ "group": "bus", "normal": [0, 0, 1], "spec_exp": 0.0, "weight": 1.0 }   // group route
{ "attitude": "SunTracking", "spec_exp": 12.0, "weight": 0.3 }            // legacy route
```

| Surface field | Meaning |
|---|---|
| `group` | Name (or index) of an entry in this type's `attitude_groups`. |
| `normal` | Surface normal in that group's body frame. Default `[0,0,1]`. |
| `attitude` | Legacy orientation mode — converted to an attitude group at load, so the two routes converge. |
| `spec_exp` | Phong exponent: `0` Lambertian, `18` a sharp panel flash, `200` a sub-degree spike. |
| `weight` | Contribution relative to the primary, which is always 1.0. Secondary surfaces are typically 0–0.5. |

**Mounting a surface on a group changes only its orientation.** Brightness still comes from the legacy
photometric fields, so a group-route type is still the two-surface Phong model — with a body-frame
normal that can follow a real hinge instead of one of the fixed legacy attitudes. Real mirror
behaviour (a physical specular peak from geometry) is a geometry-model feature: that is what the
`mirror` material preset is for.

### Legacy attitude modes

| Value | Surface normal | Typical use |
|---|---|---|
| `NadirPointing` | Toward the Earth's centre | Antenna / phased-array face (Starlink) |
| `SunTracking` | Toward the Sun | Solar arrays (ISS, OneWeb) |
| `Tumbling` | Random spin about a fixed body axis | Debris, defunct satellites |
| `Perpendicular` | `cross(primary_normal, nadir)` — secondary only | Along-track radiators |
| `AntiNadir` | Away from the Earth's centre — secondary only | Deep-space radiators |
| `FlatMirror45` | `normalize(sunDir + nadir)` | Reflects sunlight straight down |
| `TargetedReflector` | `normalize(sunDir + toTarget)` | Aims a beam at the nearest night-side ground site |
| `KnifeEdge` | Rolls about the velocity axis to put the Sun edge-on; clamped to ±80° | Brightness mitigation with the arrays still making power |
| `SunPerp` | `cross(sunDir, nadir)` | Thermal radiator always edge-on to the Sun (no direct irradiance; contributes via `diffuse`) |
| `SunTrackingTilted` | `SunTracking` pitched away from nadir toward zenith by the global *Flare mitigate tilt* setting | Steers reflected glare away from ground observers; power drops by `cos(tilt)` |

These are **converted to attitude groups at load** — the GPU never sees them — and the conversion is
written out in `satellite_types_resolved.json`, which is the easiest way to see what a legacy mode
became. New content should use groups directly.

### Attitude groups

A group is a rigid body frame. Read one as: *"this frame's `axis` points at `target`, then its roll is
fixed by a second pair, then an optional joint turns it."* Surfaces (`group` + `normal`) and model
components mount on it — so a group is really "the part of the satellite that holds still relative to
each other".

```jsonc
{
  "name": "bus",
  "law": "two_vector",                       // or "tumble" for uncontrolled spin
  "primary":   { "axis": [0, 0, 1], "target": "nadir" },
  "secondary": { "axis": [1, 0, 0], "target": "velocity" }
}
```

| Field | Meaning |
|---|---|
| `name` | Referenced by a surface's or component's `group`. |
| `law` | `two_vector` (default) or `tumble`. A tumbling body spins about the satellite's own random axis — per satellite rate and phase — with body `+Z` as the spin axis and `+X` sweeping the plane perpendicular to it. |
| `primary.axis` / `.target` | Body axis (default `[0,0,1]`) that points **exactly** at its target. Default target `nadir`. |
| `secondary.axis` / `.target` | Body axis (default `[1,0,0]`) that points **as close as possible** at its target — this is what fixes the roll about the primary. Must not be parallel to the primary axis. Default target `velocity`. |

Targets (`att_target`):

| Value | Direction |
|---|---|
| `nadir` / `zenith` | Toward / away from the Earth's centre |
| `sun` / `anti_sun` | Toward / away from the Sun |
| `velocity` / `anti_velocity` | Along / against the orbital velocity |
| `orbit_normal` / `anti_orbit_normal` | The orbit's angular-momentum axis, and its anti |
| `sun_reflect_nadir` | The normal that reflects sunlight straight down |
| `sun_reflect_ground_site` | The normal that reflects sunlight onto this satellite's chosen ground site (the Reflect Orbital lock-window machinery, fed by `reflector_targets.json`) |

A group can be a **child** of another group, which is what turns a type into a kinematic tree:

```jsonc
{
  "name": "array",
  "parent": "bus",                                  // parent must be declared before the child
  "hinge": { "position": [0, 0, -0.1], "axis": [0, 1, 0] },   // in the PARENT's body frame
  "joint": { "mode": "track", "vector": [1, 0, 0], "target": "sun" }
}
```

| Joint field | Meaning |
|---|---|
| `mode` | `none`, `track` (turn so `vector` points as close to `target` as it can), `edge_on` (turn so `vector` is perpendicular to `target` — the smaller solution), `fixed` (a constant `angle_deg`), or `flare_mitigation_tilt` (the global *Flare mitigate tilt* setting). |
| `axis` | The hinge axis, in the parent's body frame. `vector` should be perpendicular to it. |
| `vector`, `target` | What the joint aims (`target` is an `att_target` above). |
| `limit_deg` | Rotation clamp for `track` / `edge_on`. Default 180. |
| `angle_deg` | The angle for `fixed`. Default 0. |

Limits: a type in `constellations.json` may declare **1–2** groups; a **model file** may declare a
tree of up to **4** (`kMaxAttitudeGroups`), parents before children. Beyond that the loader rejects
the file and says why.

---

## Geometry model files — `satellite_models/<id>.json`

```jsonc
{
  "name": "My Satellite",                 // shown in the UI
  "note": "free text: body frame, what is sourced vs estimated",   // optional, ignored by the app
  "materials":    [ /* local materials, optionally extending a preset */ ],
  "attitude_groups": [ /* the kinematic tree (see above) */ ],
  "components":   [ /* primitives mounted on those groups */ ],
  "sources":      [ /* provenance: where each number came from */ ]
}
```

The app needs `attitude_groups` (at least one) and `components` (at least one non-`render_only`). The
model is baked once at startup into a list of **facet lobes** — one per distinct (group, material,
normal) — which is what the photometry then evaluates, so more triangles do not mean more cost per
frame; more *lobes* do.

### Materials

A model-local material extends a **preset** and can override any of its fields:

```jsonc
{ "name": "irosa_cell", "preset": "solar_cell_flex", "color": [0.12, 0.14, 0.3] }
{ "name": "truss_lattice", "preset": "truss", "coverage": 0.5, "truss_pitch": 2.3, "diffuse_albedo": 0.6 }
```

| Field | Meaning |
|---|---|
| `name` | Required. Referenced by a component's `material` / `back_material`. |
| `preset` | Base preset name (table below). Unknown preset names warn and leave the defaults in place. |
| `diffuse_albedo` | ρ_d, the Lambertian albedo. |
| `specular_f0` | F0 — normal-incidence specular reflectance (0.04 for paint/glass, 0.9 for a metal or mirror). |
| `roughness` | GGX α (or Beckmann α when `distribution` is `beckmann`). `0.0005` is a mirror, `0.5` a matte surface. |
| `color` | Linear RGB, used by the renderer for albedo/specular tint. |
| `distribution` | `ggx` (default) or `beckmann`. Beckmann has Gaussian tails; a smooth dielectric stack (a dielectric mirror film) has no long tail, and GGX's tail forward-scatters grazing terminator sunlight enough to matter photometrically. |
| `pattern` | Procedural surface detail — **visual only, photometrically neutral** (each pattern is a box filter with an area mean of exactly 1). `none`, `solar_cells`, `mli`, `panel_seams`, `truss`. Unset = inferred from the preset (solar-cell presets → cells, `mli_foil` → mli, `truss` → truss; `panel_seams` is always explicit). |
| `coverage` | 0–1 for **open lattices**: the fraction of the primitive's area that is actually material. A closed primitive also gains inner faces at `coverage·(1 − coverage)`, and the part stops shadowing anything (light goes through it). `truss` ships at 0.3. |
| `truss_pitch` | Bay pitch in metres for the lattice cut-out. Minimum 0.05. |
| `transmission` / `transmission_color` | Diffuse **transmission** through a translucent part: light landing on the far side leaves this side tinted and diffuse (backlit solar arrays on a Kapton blanket). In the photometry from the start — a backlit array is not a guess. |

Presets (`diffuse_albedo`, `specular_f0`, `roughness`, `color`; α is GGX except where noted):

| Preset | ρ_d | F0 | α | Notes |
|---|---|---|---|---|
| `solar_cell` | 0.02 | 0.04 | 0.05 | Cover glass over dark cells. Albedo calibrated on VisorSat. |
| `solar_cell_flex` | 0.02 | 0.04 | 0.05 | As above with `transmission` 0.05 (ISS iROSA-style flexible array). |
| `solar_array_back` | 0.50 | 0.04 | 0.30 | The white/Kapton back of an array. |
| `solar_array_flex_back` | 0.30 | 0.04 | 0.35 | Translucent blanket back: amber, `transmission` 0.05. |
| `array_backsheet_dark` | 0.08 | 0.04 | 0.30 | Opaque dark-pigmented backsheet (V2 Mini). |
| `mli_foil` | 0.20 | 0.60 | 0.35 | Crinkled metallised insulation; warm gold, very rough. |
| `white_paint` | 0.80 | 0.04 | 0.50 | Module hulls, radiators. |
| `black_paint` | 0.05 | 0.04 | 0.40 | Unpublished dark hardware. |
| `aluminum` | 0.10 | 0.90 | 0.20 | Bare metal structure. |
| `stainless_steel` | 0.05 | 0.55 | 0.12 | Bare 304L tank walls (Starship-class); steel-tinted specular. |
| `antenna_panel` | 0.05 | 0.04 | 0.10 | Flat phased-array face. |
| `osr_radiator` | 0.05 | 0.90 | 0.01 | Silvered quartz optical solar reflector — nearly a mirror. |
| `mirror` | 0.00 | 0.95 | 0.0005 | Large flat mirror (Reflect Orbital class). |
| `dielectric_mirror_gen1` | 0.030 | 0.90 | 0.03 | Starlink Gen1 nadir film. **Beckmann.** |
| `dielectric_mirror_gen2` | 0.003 | 0.90 | 0.03 | V2 Mini film: 10× smaller diffuse floor. **Beckmann.** |
| `truss` | 0.30 | 0.60 | 0.30 | Anodised/bare aluminium members, `coverage` 0.3, lattice cut-out. |

### Components

One component is one primitive with one material, mounted on one group.

| `type` | Required fields | Notes |
|---|---|---|
| `plane` | `size` `[w, h]` | Normal is `+Z`. The back face uses the same material unless `back_material` is set; `single_sided: true` drops it entirely. |
| `box` | `size` `[x, y, z]` | |
| `cylinder` | `radius`, `height` | Axis along `Z`. `caps` (default `true`) adds the end discs. |
| `cone` | `radius_bottom`, `height` | `radius_top` defaults to 0 (a true cone). `caps` as above. |
| `sphere` | `radius` | |

| Placement field | Meaning |
|---|---|
| `group` | The group this component belongs to (name or index). Defaults to group 0 if omitted. |
| `material` | Required. A model-local material name, or a preset name. |
| `back_material` | Optional material for the other face of a plane. |
| `position` | Relative to **the group's hinge** (not the satellite origin), in that group's body frame. |
| `rotation_quat` `[w,x,y,z]` | Orientation. |
| `rotation_deg` `[x,y,z]` | Orientation in degrees, applied intrinsically X→Y→Z (`R = Rx·Ry·Rz`). |
| `pivot` | Child groups only: see below. |
| `render_only` | `true` = **drawn and nothing else** — no facets, no lobes, no occluder, no `sources` entry. Free greebles: they cost render triangles, not photometry. The loader moves them to the end of the component list so occluder *i* still means component *i* for every consumer. A model cannot be entirely `render_only`. |

**`pivot` — one joint, several parallel axes.** On a component of a *child* group, `pivot` is a point
relative to the group's hinge (like `position`). The joint then turns the component about the parallel
axis through `hinge + pivot` instead of through the hinge itself. That is how the ISS's four beta
gimbals live in one group, and how a component can pivot on a group it is not itself a child of.
Normals — so every lobe and every magnitude — are unchanged; only the posed *position* shifts.

### Worked example

A minimal type: a white bus, and one solar wing that tracks the Sun about its hinge.

```jsonc
{
  "name": "Example bus + wing",
  "materials": [
    { "name": "bus_white", "preset": "white_paint" },
    { "name": "cell",      "preset": "solar_cell", "pattern": "solar_cells" },
    { "name": "blanket",   "preset": "solar_array_flex_back" }
  ],
  "attitude_groups": [
    { "name": "bus",
      "primary":   { "axis": [0, 0, 1], "target": "nadir" },
      "secondary": { "axis": [1, 0, 0], "target": "velocity" } },
    { "name": "wing", "parent": "bus",
      "hinge": { "position": [0.6, 0, 0], "axis": [0, 1, 0] },
      "joint": { "mode": "track", "vector": [1, 0, 0], "target": "sun" } }
  ],
  "components": [
    { "name": "bus",  "type": "box",   "group": "bus",  "size": [1.0, 1.0, 1.2],
      "material": "bus_white" },
    { "name": "wing", "type": "plane", "group": "wing", "size": [6.0, 2.0],
      "position": [3.0, 0, 0], "material": "cell", "back_material": "blanket" }
  ]
}
```

With no `sources` block this is fine for a personal mod, and `SatModelTool` will tell you exactly which
parts it could not attribute (see *Tooling*).

### Provenance — the `sources` block

Every group, component and model-local material is expected to appear in `sources`:

```jsonc
{ "subject": "solar_array", "status": "derived", "value": "one 2.8 x 8.1 m array",
  "source": "McDowell, planet4589.org/astro/starsim" }
```

| Field | Meaning |
|---|---|
| `subject` / `subjects[]` | One name, or several names that share one entry. |
| `status` | `sourced` (a document says so), `derived` (follows from a sourced dimension), `estimate` (unpublished — say why it is plausible), `calibrated` (fitted to a benchmark; name the benchmark, the metric, and what stays held out). |
| `value` | What the entry actually asserts. |
| `source` | Where it comes from. |

This is what keeps a model reviewable: `SatModelTool` prints the counts, every non-`sourced` entry and
any part with no entry at all, and a model is only accepted as a **benchmark reference** with zero
unexplained parts. The app parses the block but does not act on it — it exists for people and for the
tools.

---

## Constellations — the orbital shells

Each entry in `constellations` is one shell: one type, one altitude, one pattern, many satellites.

```jsonc
{
  "name": "My Constellation",     // shown in the Settings panel
  "type": "My Satellite",         // must match a satellite_types "name" exactly
  "alt_km": 550.0,
  "incl_deg": 53.0,
  "num_planes": 20,
  "per_plane": 30,
  "enabled": true,
  "distribution": "Walker"
}
```

| Field | Meaning |
|---|---|
| `name` | Display name in the Settings panel. |
| `type` | A `satellite_types` name. Case-sensitive, exact. |
| `alt_km` | Orbital altitude above the surface. LEO 200–2000, MEO 2000–35786, GEO ≈ 35786. |
| `incl_deg` | Inclination. Walker: fixed for every plane. RandomShell: the **maximum** (satellites are drawn uniformly from 0 to this). Ignored when `align_terminator` is true. |
| `num_planes` | Walker/Disk: number of planes. RandomShell: not a plane count — it only contributes to the total. |
| `per_plane` | Satellites per plane (Walker/Disk) or per unit (RandomShell). |
| `enabled` | Whether the shell is on at startup; toggleable in Settings at runtime. Default `true`. |
| `distribution` | `Walker` (default), `RandomShell`, or `Disk`. |
| `alt_jitter_km` | Per-satellite altitude scatter ±km. RandomShell and Disk only; Walker ignores it. |
| `raan_deg` | Disk: the plane's Right Ascension of the Ascending Node. |
| `align_terminator` | Disk: override `incl_deg` and `raan_deg` to put the plane on the Earth–Sun terminator and precess it at the SSO rate. |
| `num_rings` | Disk: concentric rings, centred on `alt_km`. Default 1 (a single ring). |
| `ring_spacing_km` | Disk: altitude step between rings. The total span is `(num_rings − 1) × ring_spacing_km`. |

**Total satellites = `num_planes` × `per_plane`, for every distribution.**

| Distribution | What it generates | Usually wants |
|---|---|---|
| `Walker` | A regular Walker constellation: `num_planes` planes evenly spread in RAAN, `per_plane` satellites phased within each plane. | `incl_deg`, `num_planes`, `per_plane` |
| `RandomShell` | Random RAAN and random inclination up to `incl_deg`, plus `alt_jitter_km`. | Debris fields, mixed-inclination shells |
| `Disk` | Concentric rings of satellites in one plane — the flat "starlink disk" look from orbit. | `raan_deg` (or `align_terminator`), `num_rings`, `ring_spacing_km` |

`align_terminator: true` computes the sun-synchronous inclination from the J2 formula *at that shell's
altitude*, anchors the RAAN to the dawn–dusk terminator and precesses it at roughly one revolution per
year. The shell then keeps the same relationship to the day/night boundary no matter what the
simulation date is — which is what a real SSO does. `incl_deg` and `raan_deg` are ignored.

---

## Adding things — checklists

### A new population from an existing type (the common case)

1. Add a `constellations` entry whose `type` is an existing type name.
2. Restart.

### A new satellite type, geometry route

1. Write `data/satellite_models/<id>.json`: `materials`, `attitude_groups`, `components`. Start from a
   shipped model of a similar shape — `starlink_v1_5` (flat bus + Sun-tracking wing), `hubble` (a tube
   with arrays), `iss` (a four-group tree with parallel gimbals), `reflect_orbital` (a mirror aimed at
   a ground site).
2. Check it with `SatModelTool`: the lobe table, the validator against a brute-force triangle
   evaluation, and `--out` for the OBJ if you want to look at the shape.
3. Add the type to `constellations.json`: `{ "name": "My Satellite", "model": "my_satellite" }`.
4. Add a shell that references it, restart, and compare the trace window's predicted magnitude with
   what you expected.

### A new satellite type, legacy route

1. Add the type with `base_color`, `cross_section_m2`, `primary` (and optionally `secondary`,
   `diffuse`, `mirror_frac`).
2. Add a shell that references it, restart.

### The copy trap when editing a shipped file

The app reads the files **next to the executable**, and `cmake --build` re-copies
`data/constellations.json`, `constellations.schema.json`, `reflector_targets.json` and the whole
`data/satellite_models/` directory over them. So:

- edit `<exe>/…` and do not rebuild → your edit wins;
- edit `data/…` in the source tree and rebuild → your edit wins;
- edit `<exe>/…` and then rebuild → the source copy overwrites your edit.

Pick one and stay there. In a release archive, next to the exe is the only copy that exists.

## Tooling

| What | Command |
|---|---|
| Build the tool | `cmake --build build --target SatModelTool` |
| Inspect a model | `build/Debug/SatModelTool.exe data/satellite_models/hubble.json` — lobe table, budgets, materials, provenance counts |
| Export the shape | add `--out <dir>` (the app also writes one to `<user data>/satellite_models_debug/` at startup) |
| Force a lobe budget | `--budget <lobes>` |
| Study self-shadowing | `--shadow-study <N>` |
| Model selftests | `--selftest <N>` — known-value checks on the photometric evaluator |
| Check a published dataset | `--benchmark data/benchmarks/<file>.json` |
| Simulate a published campaign | `--run-benchmark <file> [--samples N] [--seed S] [--sensitivity] [--report-dir D]` |
| Replay a trace | `--replay-trace <trace.csv> [--models-dir D]` |
| Try a material tweak | `--set [<model>/]<material>.<field>=<value>` |
| The whole accuracy gate | `cmake --build build --target accuracy-gate` — every model selftest and all 9 benchmark datasets; this is what CI runs |

Inaccuracies that are accepted rather than fixed are logged, with the measurement behind them, in
`data/benchmarks/KNOWN_RESIDUALS.md`. Read that before "fixing" a number.

---

## Limits and performance

| Limit | Value | Notes |
|---|---|---|
| Satellites in one roster | 10,000,000 (`MAX_SATELLITES`) | GPU buffers are sized to the roster that actually loaded (~100 B per satellite, ≈ 131 MB for the default ~1.38M), not to the cap. |
| Constellations in the Settings panel | 256 | Every shell loads and renders; only the first 256 get a toggle button. |
| Attitude groups | 2 in `constellations.json`, 4 in a model (`kMaxAttitudeGroups`) | A model's groups form a tree; parents must come first. |
| Lobe budget | 48 per type — 256 while the roster is ≤ 10,000 satellites | Flat-panel types use about 10 lobes. A curved or greebled type is merged down to the budget, which costs magnitude accuracy (see `KNOWN_RESIDUALS.md`). |
| Satellites drawn as meshes | 4096 instances | Anything large enough on screen becomes a 3D mesh; past that cap the rest stay point sprites. |

What ships by default (`data/constellations.json`) is 24 types — 23 geometry models plus one legacy
comparison type — across 28 shells, about 1.38 million satellites.

---

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| The simulation ignores the whole file | A JSON syntax error: it falls back to the built-in defaults and warns. |
| A shell never appears | Its `type` does not exactly match a `satellite_types` name (case-sensitive), or `enabled` is false. |
| A type is missing or oddly faint | Its model failed to load (check the warnings) and the type fell back to legacy fields it may not have. |
| Editing `cross_section_m2` changes nothing | Correct for a `"model"` type: the legacy brightness fields are ignored. Edit the model instead. |
| An edit has no effect at all | The copy trap above — you edited the source copy without rebuilding, or the exe copy and then rebuilt. |
| Load rejected with a group error | A component or surface names a group that does not exist, more than 4 groups, or a child declared before its parent. |
| Everything disappeared at once | One bad entry, and the file fell back to built-in defaults. Diff against `data/custom/constellations_v1_1_default.json`. |
| A gimbal turns the wrong way or through the bus | `vector` is not perpendicular to the hinge `axis`, or `limit_deg` allows more travel than the real mechanism has. |
| A mirror never glints | `sun_reflect_ground_site` needs a reachable site in `reflector_targets.json`, and a *physical* specular peak comes from a `mirror`-class material — not from `spec_exp`. |

---

## For developers

Adding a field to a satellite type or a constellation shell:

1. Add it to the C++ struct with a sensible default and read it with
   `jt.value("new_field", default_value)` — missing keys silently take the default, so older files
   keep loading.
2. Update the hardcoded fallback roster in `SatelliteSim.cpp` if the field matters there.
3. Add it to `data/constellations.schema.json` **with a `description`** (that text is what users see
   as a tooltip) and to the relevant `additionalProperties: false` list.
4. Set it in `data/constellations.json` where it applies, and in `data/custom/` if it makes a good
   example.
5. Update this file. If the *resolved* form of a type changes, say so in the changelog — people paste
   from `satellite_types_resolved.json`.

Changing a preset or any material number changes photometry: run
`cmake --build build --target accuracy-gate` before and after, and record any movement in
`KNOWN_RESIDUALS.md`.