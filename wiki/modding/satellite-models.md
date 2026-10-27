# Satellite models

A satellite model is a JSON file in `satellite_models/` describing a spacecraft's geometry: primitives (planes,
boxes, cylinders, cones, spheres, bodies of revolution, fins) with physical materials, mounted on rigid attitude
groups that can turn on hinges. One file serves both the brightness model and the 3D view: at start-up it is
reduced to a set of reflection lobes for the photometry, and tessellated separately for drawing. The same format
describes satellites, stations, rockets, launch pads, landing ships and debris. This page documents the format,
the material presets, the generated models and how to check a model. How lobes and brightness are computed is on
[Satellite photometry](../simulation/photometry.md); how a mesh is drawn is on
[Satellite meshes](../rendering/satellite-meshes.md).

A model is used by a satellite type in `constellations.json`: `{ "name": "...", "model": "<id>" }` loads
`satellite_models/<id>.json`. See [Constellations](constellations.md#model-types). Some models are loaded by
file name instead; see [Models loaded by name](#models-loaded-by-name).

## File structure

```json
{
  "name": "Example bus + wing",
  "note": "free text, ignored",
  "deployment":      { "unfold_delay_s": 600, "unfold_s": 240 },
  "materials":       [ ... ],
  "attitude_groups": [ ... ],
  "components":      [ ... ],
  "docking_ports":   [ ... ],
  "sources":         [ ... ]
}
```

| Key | Required | Meaning |
|---|---|---|
| `name` | no (defaults to the id) | Display name |
| `note` | no | Free text for people; ignored |
| `deployment` | no | How the satellite unfolds after leaving its launcher (see [Stowed for launch](#stowed-for-launch)) |
| `materials` | no | Model-local materials, usually extending a preset |
| `attitude_groups` | yes, 1 to 4 | The rigid frames the parts are mounted on |
| `components` | yes, at least one not `render_only` | The parts |
| `docking_ports` | no | Where visiting spacecraft dock (stations; see [Docking ports](#docking-ports)) |
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
- A component's `position` is measured from its group's origin: the satellite's origin for a root group, the
  hinge point for a child group. For most primitives it is the primitive's centre; for a `profile` it is the
  origin of the profile's own z coordinates, and for a `fin` the middle of its root chord.

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
| `profile` | `profile: [[z, r], ...]` | A body of revolution about Z through the listed meridian points (below) |
| `fin` | `root_chord`, `span` | A tapered, swept, thick fin (below) |

**Profiles** describe noses, domes, fairings, boattails, tanks and engine bells. `profile` lists at least two
`[z, r]` points (radius at least 0) along the **outside** of the surface, from the bottom to the top, so the
facet normals point outward; z is measured from `position` along the local Z axis. The photometry turns it in 32
steps of azimuth; the render mesh uses facets of about 0.3 m (12 to twice the base count) with smooth normals,
except across a crease of more than 35° between neighbouring segments (a dome's rim, a bell's lip). Its shadow is
the exact cone frustum through its two end radii. Options:

| Field | Default | Meaning |
|---|---|---|
| `caps` | false | Flat end discs: `true` / `"both"`, `"bottom"` (the lower-z end), `"top"`, or `false` |
| `arc_material` + `arc_deg: [start, end]` | none | A second material over an azimuth range, in degrees from local +X toward +Y (end > start; the range may cross 360): a heat shield on the windward half |
| `inner_material` | none | Draw the inside too, in this material: an open engine bell. Render mesh only; the photometry has no inner faces |
| `sweep_deg: [start, end]` | `[0, 360]` | A partial body of revolution (a fairing half). No caps allowed with a partial sweep. Its shadow stays the full frustum |

**Fins** are flaps, strakes and grid-fin frames. The root chord lies along the local Z axis, centred on
`position`, with the leading edge toward +Z; the span runs along local +X and the thickness along local Y.

| Field | Default | Meaning |
|---|---|---|
| `root_chord` | required | Chord at the root, metres |
| `tip_chord` | `root_chord` | Chord at the tip |
| `span` | required | Length along +X |
| `sweep` | 0 | How far the tip's leading edge sits aft of the root's (positive = swept back) |
| `thickness` | 0.05 × `root_chord` | Thickness at the root, along Y |
| `tip_thickness` | `thickness` × `tip_chord` / `root_chord` | Thickness at the tip |
| `bevel` | 0.15 (clamped to 0.45) | The share of the chord over which each edge thins to a knife edge |

`back_material` paints a fin's faces toward local −Y (a flap's tiled windward side). A fin's shadow is its
bounding box.

!!! tip "Profile slopes"
    The lobe bake merges faces whose normals are within about half a degree. Keep neighbouring meridian
    segments either equal in slope or more than 0.5° apart, or two almost-parallel bands merge and the exact
    bake check in SatModelTool's self-test fails.

### Component fields

| Field | Type | Default | Meaning |
|---|---|---|---|
| `name` | string | `component<index>` | Used by `sources` and in reports |
| `type` | string | required | One of the primitives above |
| `material` | string | required | A model-local material or a preset name |
| `back_material` | string | the front material | Plane: the material of the back face. Fin: the −Y faces |
| `single_sided` | boolean | false | Plane only: no back face at all |
| `caps` | boolean | true (profile: false) | Cylinder and cone: close the ends. Profile: see above |
| `group` | name or index | group 0 | The attitude group it is mounted on |
| `position` | `[x, y, z]` | `[0, 0, 0]` | Where it sits, from the group's origin (see [Coordinates](#coordinates-and-units)) |
| `rotation_quat` | `[w, x, y, z]` | identity | Orientation as a quaternion (normalised on load) |
| `rotation_deg` | `[x, y, z]` | `[0, 0, 0]` | Orientation as angles, applied intrinsically X, then Y, then Z. Ignored when `rotation_quat` is given |
| `pivot` | `[x, y, z]` | `[0, 0, 0]` | Child groups only: turn this part about a parallel axis through hinge + `pivot` (below) |
| `render_only` | boolean | false | Drawn only; no effect on brightness (below) |
| `stow` | object | none | Packed for launch; unfolds after release ([Stowed for launch](#stowed-for-launch)) |
| `fold` | object | none | Folded about a hinge line for launch ([Stowed for launch](#stowed-for-launch)) |
| `since` | `"YYYY-MM-DD"` | none (always there) | The date the part is built ([Built by date](#built-by-date)). A malformed date fails the model |

**Render-only parts.** A component with `render_only: true` is drawn in the 3D view and does nothing else: no
lobes, no shadowing, no provenance entry needed. Use it for greebles (antennas, trusses, thrusters, engine bells)
whose photometric effect is negligible: they cost render triangles, not per-frame brightness work. Its light also
seeds neither the bloom nor the glare: those are scaled by the photometric model's intensity, which has no
render-only parts in it, so a sunlit fitting cannot flare on its own. A model made only of render-only parts is
rejected.

**Pivots: one joint, several axes.** On a component of a child group, `pivot` moves the axis it turns about: the
joint turns it about the line parallel to the hinge axis through hinge + `pivot`, instead of through the hinge.
Several parts of one group can then turn by the same angle about different parallel axes, like a station's four
array gimbals driven together, or four wings each turning about its own long axis. Normals, and so brightness,
are unchanged; only where the part sits moves. A pivot on a root-group component is an error.

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
joint with a zero axis, and a `track`, `edge_on` or `edge_on_continuous` joint whose `vector` is parallel to its
axis.

For a joint that aims a vector, `vector` should be perpendicular to the hinge axis (the component along the axis
is ignored), and `limit_deg` should match the real mechanism's travel. For a radiator that must stay edge-on to
the Sun, prefer `edge_on_continuous` to `edge_on`: `edge_on` takes the smaller of the two edge-on solutions, which
swaps sides twice an orbit (the radiator flips 180° in a moment), while `edge_on_continuous` keeps one solution
and turns smoothly once per orbit, as the ISS's thermal radiators do.

Satellites whose root group uses the `tumble` law only spin in a RandomShell shell; see
[Constellations](constellations.md#the-three-distributions). The attitude laws themselves are described on
[Attitude](../simulation/attitude.md).

## Materials

A material is a diffuse term (Lambertian by default) plus a microfacet specular lobe (GGX by default, or
Beckmann), with Schlick Fresnel, and optionally a second, broader specular lobe (a haze). The equations are on
[Satellite photometry](../simulation/photometry.md#the-reflectance-of-one-lobe). A component may name a preset directly, or a model-local material:

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
| `name` | string | required | Referred to by `material` / `back_material` / `arc_material` / `inner_material` |
| `preset` | string | none | Base material. An unknown name is logged and the defaults below are used |
| `diffuse_albedo` | 0 to 1 | 0.1 | Diffuse albedo \( \rho_d \) |
| `minnaert` | 0.2 to 1 | 1 | Minnaert exponent \( k \) of the diffuse term; 1 is Lambert (below) |
| `specular_f0` | 0 to 1 | 0.04 | Reflectance at normal incidence: about 0.04 for paint and glass, 0.9 for metal and mirrors |
| `roughness` | number | 0.2 | Microfacet \( \alpha \): 0.0005 is a mirror, 0.5 matte |
| `distribution` | `ggx` or `beckmann` | `ggx` | GGX has long tails; Beckmann's are Gaussian, right for smooth dielectric films |
| `haze` | `{"fraction": f, "roughness": α}` | none | A share \( f \) (0 to 1) of the specular reflectance in a second lobe of roughness α (default 0.04), same distribution (below) |
| `color` | `[r, g, b]` | `[1, 1, 1]` | Visual tint in the 3D view and the OBJ export |
| `transmission` | 0 to 1 | 0 | Diffuse transmission: the share of light falling on the other side that leaves this side, diffusely |
| `transmission_color` | `[r, g, b]` | `[1, 0.55, 0.15]` | Tint of transmitted light in the 3D view (amber, as through Kapton) |
| `coverage` | (0, 1] | 1 | Open lattice: the solid fraction of the surface (below) |
| `truss_pitch` | metres, ≥ 0.05 | 1 | Bay size of the lattice cut-out |
| `pattern` | see below | from the preset | Procedural surface detail, visual only |

**Minnaert diffuse.** With `minnaert` below 1 the diffuse intensity is \( \rho_d (k+1)/(2\pi)\,(\cos i \cos e)^k \)
per unit area instead of Lambert's \( \rho_d \cos i \cos e / \pi \): the same with light and view overhead, brighter
than Lambert with either grazing. Rough and painted surfaces often behave so; Mallama measured \( k \approx 0.58 \)
for the original Starlink. The value is stored at a resolution of 1/127, and a value outside [0.2, 1] is logged and
replaced by 1. Transmission stays Lambertian. The 2020 Starlink models use 0.65.

**Haze.** A silvered or polished film puts most of its reflection in a sharp core and scatters a share into wide
wings around it, which one lobe cannot do. With `"haze": {"fraction": 0.3, "roughness": 0.04}` the material
reflects 70% of its specular light through its own `roughness` and 30% through a lobe of α 0.04. Each hazy facet
group bakes into two lobes, so it counts twice against the lobe budget. A fraction outside [0, 1] is logged and
replaced by 0, a roughness outside (0, 1] by 0.04. `iridium_block1.json` uses it on its Main Mission Antennas.

**Transmission** models a translucent part such as a flexible solar array on a Kapton blanket: a backlit array
glows through the gaps between its cells. It is part of the brightness model, not only the picture.

**Open lattices.** With `coverage` below 1, a part is a truss rather than a solid: each face counts only
`coverage` of its area, a closed primitive also shows its inner far faces through the gaps (at
`coverage × (1 − coverage)`), and the part never shadows anything. The 3D view cuts the members out of the surface
on a bay grid with diagonals, with the same mean coverage. A value outside (0, 1] is logged and replaced by 1.

**Patterns** add visible surface detail in the 3D view: `none`, `solar_cells` (a cell grid with lighter gaps),
`mli` (crinkled insulation foil), `panel_seams` (panel joints every 0.5 m), `truss` (the lattice cut-out),
`hex_tiles` (heat-shield tiles: 0.31 m hexagons with thin off-white gaps, a shade per tile and a few pale
replacement tiles). Each pattern averages to exactly 1 over the surface, so it never changes the brightness. When
`pattern` is not set it is inferred: `solar_cell`, `solar_cell_flex` and `solar_array_flex_back` get
`solar_cells`, `mli_foil` gets `mli`, `truss` gets `truss`, everything else `none`.

**Material names the renderer recognises.** Two local material names change how a part is drawn, whatever its
fields:

| Name | Effect |
|---|---|
| `soot` | The inside of an engine bell: it glows (orange at the lip to yellow-white at the throat) while its stage burns |
| `gridfin` | With `coverage` below 1: a square grid of plates of `truss_pitch` cells with no diagonals, drawn solid at its mean once a cell is a few pixels across |

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

## Stowed for launch

A satellite carried by a launch (see [Launches](../simulation/launches.md)) leaves its dispenser packed and
unfolds afterwards. Satellites in the roster's slots are always fully deployed; only those still flying out of a
launch are drawn and lit part-unfolded. The model says how, with two component fields and one model field.

**`deployment`** (model level) gives the timeline. The unfolding progress P runs from 0 (as launched) to 1
(deployed) over `unfold_s`, starting `unfold_delay_s` after the satellite's release, each satellite's delay
varied by ±15 %. Without the block (or with `unfold_s` 0) the satellite arrives deployed.

**`stow`** packs a part along an axis, like an accordion array or a folded radiator:

| Field | Default | Meaning |
|---|---|---|
| `axis` | required | The direction it packs along (rest body frame) |
| `root` | 0 | The plane `dot(axis, x) = root` it packs toward |
| `scale` | 0.05 | Its extent beyond that plane when fully stowed, as a share of the deployed extent (1 = never stowed) |
| `phase` | `[0, 1]` | The window of P over which it unfolds (rounded to 1/255) |

The part's extent beyond the root plane is scaled by `mix(scale, 1, smoothstep(p0, p1, P))`. The brightness
follows it: every lobe the part contributes to loses the matching share of its area, on the GPU and in the CPU
evaluator alike.

**`fold`** turns a part about a hinge line after the stow scaling: an array pack folded flat over the bus, or half
a bus folded over the other.

| Field | Default | Meaning |
|---|---|---|
| `point` | `[0, 0, 0]` | A point on the hinge line (rest body frame) |
| `axis` | required | The hinge line's direction |
| `angle_deg` | 0 | The fold angle when stowed; the part turns back to 0 as P passes the window |
| `phase` | `[0, 1]` | The window of P over which it unfolds |

A fold is drawn only: the brightness ignores it (a satellite is folded only inside its launcher and for a minute
or two after release). While a part is stowed or folded it casts no shadow on the rest of the mesh, since it is not
where its shadow shape says.

The shipped Starmind satellite folds its 10 m bus in half over the radiator packs (`fold`, phase `[0, 0.22]`),
then unfolds the radiators and the wing strips (`stow` along Y toward the bus edge at ±1.5 m, scale 0.03), with a
10-minute delay and a 10-minute unfold:

```json
"deployment": { "unfold_delay_s": 600, "unfold_s": 600 },
...
{ "name": "wing_strip", "type": "plane", "...": "...",
  "stow": { "axis": [0, 1, 0], "root": 1.5, "scale": 0.03, "phase": [0.4, 1.0] } }
```

How many satellites fit a launcher, and in what arrangement, is set in `launches.json` from the stowed size; see
[Launch catalogue](index.md#the-launch-catalogue-launchesjson).

## Built by date

A component with `"since": "YYYY-MM-DD"` exists from that date. With the deployment era on (the default; see
[Launches](../simulation/launches.md)), a part not built yet is not drawn, casts no shadow and adds nothing to the
brightness; lobes are never merged across different dates, so this costs nothing per frame. With the era off,
every part is always there. The shipped stations use it to grow module by module (Tiangong's modules at their real
dates, the commercial stations on their operators' published sequences).

## Docking ports

A station model lists where visiting spacecraft dock, in the **root group's body frame**. Ports are mission data
only: they have no geometry and no effect on the brightness (draw the port hardware as render-only parts).

```json
"docking_ports": [
  { "name": "node_port_fwd", "position": [1.9, 0, 0], "axis": [1, 0, 0] },
  { "name": "aft_cargo", "position": [-9.0, 0, 0], "axis": [-1, 0, 0], "use": "cargo", "since": "2022-07-24" }
]
```

| Field | Default | Meaning |
|---|---|---|
| `name` | `port` | For reports |
| `position` | `[0, 0, 0]` | The contact point |
| `axis` | `[1, 0, 0]` | The port's outward axis (normalised) |
| `use` | `crew` | The port's class: a program's `crew.ports` takes only ports of its class. `crew`, `cargo`, or any name (the ISS's berthing ports are `berth_unity` and `berth_harmony`) |
| `since` | none | The date the port's module arrives; a vehicle docks only at ports already there (a malformed date counts as always) |

Spacecraft are not drawn on the station models: the program flies real crew and cargo vehicles to these ports;
see [Crew flights](../simulation/crew-flights.md).

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
`[0, 0, 1]`, points as closely at the Sun as one axis allows. Save it as `satellite_models/example.json`, add
`{ "name": "Example", "model": "example" }` to the types in `constellations.json` and a shell that flies it, and
restart. To have it unfold after a launch, add `"deployment"` and a `"stow"` on the wing with `axis` `[1, 0, 0]`
and `root` 0.5 (the bus edge).

For real shapes, start from a shipped model of a similar layout:

| Model | Why it is a good starting point |
|---|---|
| `starlink_v1_5.json` | A flat bus with one Sun-tracking wing |
| `starlink_v2_mini.json` | Two wings with tracking limits, mirror film, dark paint, stowed arrays |
| `spacex_ai_sat.json` | Starmind: a folding bus, stowed radiators and wing strips |
| `hubble.json` | A tube with arrays |
| `iss.json` | A four-group tree with parallel gimbals (pivots), translucent arrays, an open truss, continuous edge-on radiators, docking ports |
| `tiangong.json` | A station built module by module (`since`) |
| `reflect_orbital.json` | A large mirror aimed at a ground site |
| `debris_fragment.json` | The smallest file: two plates and a box, tumbling |
| `rocket_starship_upper.json` | Profiles, fins, an `arc_material` heat shield, `hex_tiles`, engine bells with `soot` insides |

## Generated models

Many shipped models are written by Python scripts. Edit the script and re-run it, not the JSON, or the next run
silently drops the change. Each generated file says so in its `note`.

| Script | Writes |
|---|---|
| `tools/make_station_models.py` | `haven1`, `haven2`, `axiom_station`, `orbital_reef`, `ross`, `bharatiya_station`, `starlab`; and the render-only `gb_*` detail of the hand-made `iss` and `tiangong` (re-running replaces it) |
| `tools/make_rocket_models.py` | `rocket_<vehicle>_{stack,booster,upper,upper_bare,upper_empty,fairing}` and the payload dispensers `rocket_dispenser_<name>`; `--check` fails when `launches.json` disagrees with the models |
| `tools/make_crew_models.py` | `rocket_capsule_<kind>` (in orbit), `_cm` (the returning capsule) and `_chute` (under parachutes); the cargo ships, Cygnus and HTV-X with grapple fixtures on both sides; Canadarm2's `rocket_arm_canadarm2_boom` and `_wrist` |
| `tools/make_pad_models.py` | `pad_starship`, `pad_falcon`, `pad_generic` |
| `tools/make_ship_models.py` | `ship_droneship`, `ship_jacklyn`, `ship_robot` |
| `tools/make_geo_models.py` | `geo_comsat_large`, `geo_comsat_medium`, `geo_weather`, `geo_spinner` |
| `tools/make_debris_models.py` | `debris_{panel,radiator,chunk}_{s,m,l}` |

**Stations.** Every wing turns about its own long axis (a child group with a pivot per wing) and is attached to its
module by a render-only yoke or mast. Photometric hulls are split into one profile per meridian segment, so each
piece's shadow (the frustum through its end radii) matches the shape: one long domed profile would shadow as a
single cone. Ports, dishes, windows, bands and arms are render-only, a few dozen parts per station.

**Rockets** use the frame +Z toward the nose, origin at the aft end of the body (the point the flight model
tracks), one group pointing +Z along the velocity and −Y (the heat shield) toward nadir. Every engine is its own
render-only bell named `engines_*`, `upper_nozzle_*` or `side_nozzle_*`: the flame and the engine light start at
the exit plane those parts span. No surface may cut a bell: each throat sits at or below the cap or heat shield
over it.

**Capsules** have their origin at the docking port, +Z out of it. Wings that turn to the Sun (Shenzhou,
Tianzhou) are a child group `wings` hinged about the span (body Y) with a `track` joint; wings that do not turn (Soyuz MS,
Progress) face along the axis. A berthed cargo ship's grapple fixtures sit where its program's `crew.grapple`
says ([radius, depth below the port]): the flight code places the arm's tip there without reading the model.

## Models loaded by name

Files whose names start with `rocket_`, `pad_` or `ship_` are loaded at start-up whatever `constellations.json`
says, as launch vehicles, launch pads and landing ships (see [Launches](../simulation/launches.md)). The debris kit
(`debris_*`) is loaded for the Kessler scenario. Their lobe budgets are fixed: 24 for launch objects, 16 for debris.
The `geo_*` models are flown by the GEO belt's types and baked at the 48-lobe budget.

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
| `--set [<model>/]<material>.<field>=<value>` | Try a material change without editing the file (`diffuse_albedo`, `specular_f0`, `roughness`, `distribution`, `haze_fraction`, `haze_roughness`, `minnaert`) |
| `--dump-json <file>` | Write the model's triangles, lobes per budget level, validation pairs and brightness spheres as JSON (`MODEL` in the name is replaced by the model id); the technical videos read it |

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
| A radiator flips over twice an orbit | Its joint is `edge_on`; use `edge_on_continuous` |
| A part is where you expected only at rest | Its `position` is from its own group's origin; a child's origin is its hinge |
| A profile is dark or inside-out | Its points run top to bottom, or along the inside; list them bottom to top along the outside |
| A profile shadows parts beside its waist | Its shadow is the frustum through its end radii: split a domed shape into one profile per segment |
| Brightness looks wrong after adding detail | Use `render_only` for parts that should not count; check the lobe validator numbers |
| A part is missing in the sky | It has a `since` date later than the sim time, with the deployment era on |
| A model edit "does not take" | It is a generated model and the script was re-run, or the copy trap (see [Modding](index.md#how-changes-are-picked-up)) |
| A large model is less accurate in a big shell | Shells of more than 10 000 satellites limit a type to 48 lobes |

## Where in the code

- `src/simulations/SatModel.cpp`: `loadSatModel()`, `satMaterialPresets()`, `satMaterialPattern()`,
  `parseAttitudeGroupJson()`, `validateAttitudeGroups()`, `tessellateSatModel()` (`tessProfile()`, `tessFin()`),
  `bakeSatLobes()`, `satLobeStowFactor()`, `writeSatModelObj()`, `unexplainedModelParts()`.
- `src/simulations/SatModel.h`: `SatMaterial`, `SatComponent`, `SatFinShape`, `SatModel::DockPort`, `AttitudeGroup`.
- `src/simulations/SatelliteSim.cpp`: `loadModelType()` and `bakeModelType()`; the by-name loading of launch models
  in `loadDefinitions()`.
- `src/simulations/SatMesh.cpp`: the render tessellation.
- `tools/sat_model_tool/main.cpp`: SatModelTool; `tools/make_*_models.py`: the generators.
