# Satellite models

A satellite model is a JSON file in `satellite_models/` describing a satellite's geometry: simple primitives
(planes, boxes, cylinders, cones, spheres) with physical materials, mounted on rigid attitude groups that can turn
on hinges. One file serves both the brightness model and the 3D view: at start-up it is reduced to a set of
reflection lobes for the photometry, and tessellated separately for drawing. This page documents the format,
the material presets, and how to check a model. How lobes and brightness are computed is on
[Satellite photometry](../simulation/photometry.md); how a mesh is drawn is on
[Satellite meshes](../rendering/satellite-meshes.md).

A model is used by a satellite type in `constellations.json`: `{ "name": "...", "model": "<id>" }` loads
`satellite_models/<id>.json`. See [Constellations](constellations.md#model-types).

## File structure

```json
{
  "name": "Example bus + wing",
  "note": "free text, ignored",
  "materials":       [ ... ],
  "attitude_groups": [ ... ],
  "components":      [ ... ],
  "sources":         [ ... ]
}
```

| Key | Required | Meaning |
|---|---|---|
| `name` | no (defaults to the id) | Display name |
| `note` | no | Free text for people; ignored |
| `materials` | no | Model-local materials, usually extending a preset |
| `attitude_groups` | yes, 1 to 4 | The rigid frames the parts are mounted on |
| `components` | yes, at least one not `render_only` | The parts |
| `sources` | no | Provenance of every number: where it came from |

Any missing required field, unknown component type, unknown material or group name, or invalid group setup makes
the whole model fail to load. The type then falls back to its legacy fields and the reason is in
`satlight_log.txt`.

## Coordinates and units

- Lengths are in metres, angles in degrees.
- Each group has a **body frame**. In the rest pose (every joint at zero) all groups share the first root
  group's axes, so every position and axis in the file is written in one frame.
- By convention +Z is the axis the root group points at its primary target (for a nadir-pointing satellite, +Z
  points down at the Earth).
- A component's `position` is the centre of its primitive, measured from its group's origin: the satellite's
  origin for a root group, the hinge point for a child group.

## Components

One component is one primitive in one material, attached to one group.

```json
{ "name": "wing", "type": "plane", "group": "wing", "size": [6.0, 2.0],
  "position": [3.0, 0, 0], "material": "cell", "back_material": "blanket" }
```

### Primitives

| `type` | Required fields | Shape (before rotation) |
|---|---|---|
| `plane` | `size: [w, h]` | A `w` × `h` rectangle in the XY plane, front face toward +Z |
| `box` | `size: [x, y, z]` | A box centred on `position` |
| `cylinder` | `radius`, `height` | Axis along Z, centred, 32 facets around |
| `cone` | `radius_bottom`, `height` | A frustum along Z: `radius_bottom` at −Z, `radius_top` (default 0, a true cone) at +Z |
| `sphere` | `radius` | |

### Component fields

| Field | Type | Default | Meaning |
|---|---|---|---|
| `name` | string | `component<index>` | Used by `sources` and in reports |
| `type` | string | required | One of the primitives above |
| `material` | string | required | A model-local material or a preset name |
| `back_material` | string | the front material | Plane only: the material of the back face |
| `single_sided` | boolean | false | Plane only: no back face at all |
| `caps` | boolean | true | Cylinder and cone: close the ends |
| `group` | name or index | group 0 | The attitude group it is mounted on |
| `position` | `[x, y, z]` | `[0, 0, 0]` | Centre of the primitive, from the group's origin |
| `rotation_quat` | `[w, x, y, z]` | identity | Orientation as a quaternion (normalised on load) |
| `rotation_deg` | `[x, y, z]` | `[0, 0, 0]` | Orientation as angles, applied intrinsically X, then Y, then Z. Ignored when `rotation_quat` is given |
| `pivot` | `[x, y, z]` | `[0, 0, 0]` | Child groups only: turn this part about a parallel axis through hinge + `pivot` (below) |
| `render_only` | boolean | false | Drawn only; no effect on brightness (below) |

**Render-only parts.** A component with `render_only: true` is drawn in the 3D view and does nothing else: no
lobes, no shadowing, no provenance entry needed. Use it for greebles (antennas, trusses, thrusters) whose
photometric effect is negligible: they cost render triangles, not per-frame brightness work. A model made only of
render-only parts is rejected.

**Pivots: one joint, several axes.** On a component of a child group, `pivot` moves the axis it turns about: the
joint turns it about the line parallel to the hinge axis through hinge + `pivot`, instead of through the hinge.
Several parts of one group can then turn by the same angle about different parallel axes, like a station's four
array gimbals driven together. Normals, and so brightness, are unchanged; only where the part sits moves. A pivot
on a root-group component is an error.

## Attitude groups and the kinematic tree

Groups use the format described on [Constellations](constellations.md#attitude-groups): a **root** group points
one body axis at a target and a second as close as possible at another, and may turn on a joint. A model file
adds **child** groups, which form a tree of up to four groups.

```json
"attitude_groups": [
  { "name": "bus",
    "primary":   { "axis": [0, 0, 1], "target": "nadir" },
    "secondary": { "axis": [1, 0, 0], "target": "velocity" } },
  { "name": "wing", "parent": "bus",
    "hinge": { "position": [0.6, 0, 0], "axis": [0, 1, 0] },
    "joint": { "mode": "track", "vector": [1, 0, 0], "target": "sun" } }
]
```

| Child field | Type | Default | Meaning |
|---|---|---|---|
| `parent` | string | none (a root) | The name of an **earlier** group in the list |
| `hinge.position` | `[x, y, z]` | `[0, 0, 0]` | The hinge point, from the parent's origin |
| `hinge.axis` | `[x, y, z]` | `joint.axis` | The joint's rotation axis. Overrides `joint.axis` |
| `joint` | object | none | As for a root group; without a joint, the child is rigidly attached |

A child is always its parent's frame turned about the hinge by its joint; its own `law`, `primary` and
`secondary` are ignored. A file may hold more than one root group (each with its own law). The loader rejects:
more than four groups, a parent that is not an earlier group, a root with parallel primary and secondary axes, a
joint with a zero axis, and a `track` or `edge_on` joint whose `vector` is parallel to its axis.

For a `track` or `edge_on` joint, `vector` should be perpendicular to the hinge axis (the component along the axis
is ignored), and `limit_deg` should match the real mechanism's travel.

Satellites whose root group uses the `tumble` law only spin in a RandomShell shell; see
[Constellations](constellations.md#the-three-distributions). The attitude laws themselves are described on
[Attitude](../simulation/attitude.md).

## Materials

A material is a Lambertian diffuse term plus a microfacet specular lobe (GGX by default, or Beckmann), with
Schlick Fresnel. A component may name a preset directly, or a model-local material:

```json
"materials": [
  { "name": "bus_white", "preset": "white_paint" },
  { "name": "irosa_cell", "preset": "solar_cell_flex", "color": [0.12, 0.14, 0.3] },
  { "name": "truss_lattice", "preset": "truss", "coverage": 0.5, "truss_pitch": 2.3, "diffuse_albedo": 0.6 }
]
```

A local material starts from its `preset` (which may also be a local material defined earlier in the list) and
overrides any field:

| Field | Type | Default (no preset) | Meaning |
|---|---|---|---|
| `name` | string | required | Referred to by `material` / `back_material` |
| `preset` | string | none | Base material. An unknown name is logged and the defaults below are used |
| `diffuse_albedo` | 0 to 1 | 0.1 | Lambertian albedo \( \rho_d \) |
| `specular_f0` | 0 to 1 | 0.04 | Reflectance at normal incidence: about 0.04 for paint and glass, 0.9 for metal and mirrors |
| `roughness` | number | 0.2 | Microfacet \( \alpha \): 0.0005 is a mirror, 0.5 matte |
| `distribution` | `ggx` or `beckmann` | `ggx` | GGX has long tails; Beckmann's are Gaussian, right for smooth dielectric films |
| `color` | `[r, g, b]` | `[1, 1, 1]` | Visual tint in the 3D view and the OBJ export |
| `transmission` | 0 to 1 | 0 | Diffuse transmission: the share of light falling on the other side that leaves this side, diffusely |
| `transmission_color` | `[r, g, b]` | `[1, 0.55, 0.15]` | Tint of transmitted light in the 3D view (amber, as through Kapton) |
| `coverage` | (0, 1] | 1 | Open lattice: the solid fraction of the surface (below) |
| `truss_pitch` | metres, ≥ 0.05 | 1 | Bay size of the lattice cut-out |
| `pattern` | see below | from the preset | Procedural surface detail, visual only |

**Transmission** models a translucent part such as a flexible solar array on a Kapton blanket: a backlit array
glows through the gaps between its cells. It is part of the brightness model, not only the picture.

**Open lattices.** With `coverage` below 1, a part is a truss rather than a solid: each face counts only
`coverage` of its area, a closed primitive also shows its inner far faces through the gaps (at
`coverage × (1 − coverage)`), and the part never shadows anything. The 3D view cuts the members out of the surface
on a bay grid with diagonals, with the same mean coverage. A value outside (0, 1] is logged and replaced by 1.

**Patterns** add visible surface detail in the 3D view: `none`, `solar_cells` (a cell grid with lighter gaps),
`mli` (crinkled insulation foil), `panel_seams` (panel joints every 0.5 m), `truss` (the lattice cut-out). Each
pattern averages to exactly 1 over the surface, so it never changes the brightness. When `pattern` is not set it
is inferred: `solar_cell`, `solar_cell_flex` and `solar_array_flex_back` get `solar_cells`, `mli_foil` gets `mli`,
`truss` gets `truss`, everything else `none`.

### Presets

\( \alpha \) is GGX unless marked Beckmann. These are estimates from published descriptions except where the
Sources column says otherwise.

| Preset | \( \rho_d \) | F0 | \( \alpha \) | Colour | Notes |
|---|---|---|---|---|---|
| `solar_cell` | 0.02 | 0.04 | 0.05 | 0.20 0.25 0.55 | Dark cells under cover glass. Diffuse albedo calibrated on the VisorSat benchmark |
| `solar_cell_flex` | 0.02 | 0.04 | 0.05 | 0.20 0.25 0.55 | As above, transmission 0.05: a flexible array on a translucent blanket |
| `solar_array_back` | 0.50 | 0.04 | 0.30 | 0.85 0.82 0.70 | White or Kapton back of a rigid array |
| `solar_array_flex_back` | 0.30 | 0.04 | 0.35 | 0.80 0.62 0.32 | Translucent blanket back, transmission 0.05 |
| `array_backsheet_dark` | 0.08 | 0.04 | 0.30 | 0.20 0.18 0.17 | Opaque, dark-pigmented backsheet |
| `mli_foil` | 0.20 | 0.60 | 0.35 | 0.95 0.78 0.40 | Crinkled metallised insulation |
| `white_paint` | 0.80 | 0.04 | 0.50 | 0.95 0.95 0.95 | Hulls, radiators |
| `black_paint` | 0.05 | 0.04 | 0.40 | 0.08 0.08 0.08 | Dark hardware |
| `aluminum` | 0.10 | 0.90 | 0.20 | 0.80 0.80 0.82 | Bare metal structure |
| `stainless_steel` | 0.05 | 0.55 | 0.12 | 0.76 0.76 0.75 | Bare stainless tank walls (Starship class) |
| `antenna_panel` | 0.05 | 0.04 | 0.10 | 0.25 0.25 0.28 | Flat phased-array face |
| `osr_radiator` | 0.05 | 0.90 | 0.01 | 0.85 0.88 0.92 | Silvered-quartz optical solar reflector: nearly a mirror |
| `mirror` | 0.00 | 0.95 | 0.0005 | 0.90 0.92 0.95 | Large flat mirror (Reflect Orbital class) |
| `dielectric_mirror_gen1` | 0.030 | 0.90 | 0.03 (Beckmann) | 0.70 0.72 0.78 | Starlink first-generation nadir mirror film |
| `dielectric_mirror_gen2` | 0.003 | 0.90 | 0.03 (Beckmann) | 0.70 0.72 0.78 | Second-generation film: a tenth of the diffuse floor |
| `truss` | 0.30 | 0.60 | 0.30 | 0.72 0.72 0.74 | Aluminium lattice, coverage 0.3 |

Changing a preset changes every model that uses it, and the benchmark results with them; see
[Results and known residuals](../accuracy/results.md).

## Provenance: the `sources` block

Every group, every photometric component and every model-local material is expected to have an entry saying
where its numbers came from:

```json
"sources": [
  { "subject": "solar_array", "status": "derived", "value": "one 2.8 x 8.1 m array",
    "source": "McDowell, planet4589.org" },
  { "subjects": ["panel_a", "panel_b"], "status": "estimate", "value": "0.25 m folded panels",
    "source": "Sized to a 0.05 m2 mean cross-section" }
]
```

| Field | Meaning |
|---|---|
| `subject` / `subjects` | One name, or several names sharing the entry |
| `status` | `sourced` (a document says so), `derived` (follows from sourced values), `estimate` (unpublished; say why it is plausible), `calibrated` (fitted to a benchmark: name it, the metric, and what stays held out) |
| `value` | What the entry asserts |
| `source` | Where it comes from |

The program reads this block but does not act on it. SatModelTool reports the counts per status, lists every
entry that is not `sourced`, and lists any part with no entry at all. A model is accepted as a benchmark reference
only with no unexplained parts. Render-only parts need no entry. See [Model provenance](../accuracy/provenance.md).

## Worked example

A white bus and one solar wing that tracks the Sun about its hinge:

```json
{
  "name": "Example bus + wing",
  "materials": [
    { "name": "bus_white", "preset": "white_paint" },
    { "name": "cell",      "preset": "solar_cell" },
    { "name": "blanket",   "preset": "solar_array_flex_back" }
  ],
  "attitude_groups": [
    { "name": "bus",
      "primary":   { "axis": [0, 0, 1], "target": "nadir" },
      "secondary": { "axis": [1, 0, 0], "target": "velocity" } },
    { "name": "wing", "parent": "bus",
      "hinge": { "position": [0.6, 0, 0], "axis": [1, 0, 0] },
      "joint": { "mode": "track", "vector": [0, 0, 1], "target": "sun" } }
  ],
  "components": [
    { "name": "bus",  "type": "box",   "group": "bus",  "size": [1.0, 1.0, 1.2], "material": "bus_white" },
    { "name": "wing", "type": "plane", "group": "wing", "size": [6.0, 2.0],
      "position": [3.0, 0, 0], "material": "cell", "back_material": "blanket" }
  ]
}
```

The wing is a 6 m × 2 m plane reaching out along +X from the hinge; at rest its cell side faces +Z, toward the
Earth. The joint turns it about the X axis (the wing's long axis) so that the cell side's normal, `vector`
`[0, 0, 1]`, points as closely at the Sun as one axis allows. Save it as `satellite_models/example.json`, add `{ "name": "Example", "model": "example" }` to the
types in `constellations.json` and a shell that flies it, and restart.

For real shapes, start from a shipped model of a similar layout:

| Model | Why it is a good starting point |
|---|---|
| `starlink_v1_5.json` | A flat bus with one Sun-tracking wing |
| `starlink_v2_mini.json` | Two wings with tracking limits, mirror film, dark paint |
| `hubble.json` | A tube with arrays |
| `iss.json` | A four-group tree with parallel gimbals (pivots), translucent arrays and an open truss |
| `reflect_orbital.json` | A large mirror aimed at a ground site |
| `debris_fragment.json` | The smallest file: two plates and a box, tumbling |

## Validating a model: SatModelTool

SatModelTool loads, bakes and checks a model without starting the program. It is not part of the default build:

```bash
cmake --build build --target SatModelTool
build/Debug/SatModelTool data/satellite_models/example.json
```

It prints the model's groups, components and materials, the baked lobe table and how far the merged lobes are
from an exact per-triangle evaluation, and the provenance counts with any unexplained part.

| Option | Use |
|---|---|
| `--out <dir>` | Write the OBJ shape files |
| `--budget <lobes>` | Bake with a different lobe budget (default 48, the budget of a large shell) |
| `--selftest <N>` | Run the brightness self-tests on N random geometries, including the part-shadowing check |
| `--shadow-study <N>` | Study how parts shadow each other |
| `--set [<model>/]<material>.<field>=<value>` | Try a material change without editing the file |

The benchmark options are described on [SatBench and the accuracy gate](../accuracy/satbench.md), and the tool
itself on [Tools](../development/tools.md).

**In the program**, every model is also checked at start-up: `satlight_log.txt` gets one line per model with its
lobe counts and the 95th-percentile and maximum magnitude error of the baked lobes against the per-triangle
evaluation, and `satellite_models_debug/<id>_rest.obj` and `<id>_sunlit.obj` (with `.mtl` files) are written to
the user data folder. The OBJ files are Y-up with the zenith up, one in the rest pose and one posed in sunlight;
open them in any 3D viewer to check that the parts sit where you meant. Then select one of your satellites in the
sky and open **Info** to see it rendered, and **Trace pass** to see its predicted brightness.

## Common mistakes

| Symptom | Cause |
|---|---|
| The type is faint and has no Info or Go to button | The model failed to load; the reason is in `satlight_log.txt` |
| "unknown material" | A component names a material that is neither a preset nor defined in `materials` |
| "parent ... must name an EARLIER group" | A child is listed before its parent, or the parent's name is misspelled |
| A wing turns the wrong way or through the bus | `joint.vector` is not perpendicular to the hinge axis, or the hinge axis is wrong, or `limit_deg` is too large |
| A part is where you expected only at rest | Its `position` is from its own group's origin; a child's origin is its hinge |
| Brightness looks wrong after adding detail | Use `render_only` for parts that should not count; check the lobe validator numbers |
| A large model is less accurate in a big shell | Shells of more than 10 000 satellites limit a type to 48 lobes |

## Where in the code

- `src/simulations/SatModel.cpp`: `loadSatModel()`, `satMaterialPresets()`, `satMaterialPattern()`,
  `parseAttitudeGroupJson()`, `validateAttitudeGroups()`, `tessellateSatModel()`, `bakeSatLobes()`,
  `writeSatModelObj()`, `unexplainedModelParts()`.
- `src/simulations/SatModel.h`: `SatMaterial`, `SatComponent`, `AttitudeGroup`.
- `src/simulations/SatelliteSim.cpp`: `loadModelType()` and `bakeModelType()`.
- `src/simulations/SatMesh.cpp`: the render tessellation.
- `tools/sat_model_tool/main.cpp`: SatModelTool.
