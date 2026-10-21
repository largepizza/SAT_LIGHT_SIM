# Satellite meshes

How satellites, rockets, launch pads and landing ships with a geometry model are drawn as 3D meshes once they
are large enough on screen: the mesh renderer and its lighting, its shadow rays, how the frame decides which
satellites get a mesh and hands each one over from its point sprite, the environment probes and sharp mirror
reflections that give the meshes the full renderer's Earth to reflect, and the model viewer in the satellite info
window. Flames, exhaust and the engine light on clouds are on [Launch visuals](launch-visuals.md). The models themselves (components,
materials, attitude groups) are described in [Satellite models](../modding/satellite-models.md) and
[Satellite photometry](../simulation/photometry.md).

## Two tessellations of one model

Every geometry model is tessellated twice:

| | Photometric | Render |
|---|---|---|
| Built by | `bakeSatLobes()` (`SatModel.cpp`) | `buildSatRenderMesh()` (`SatMesh.cpp`, 48 segments per revolved primitive by default; profiles in facets of about 0.3 m) |
| Purpose | merged facet lobes for millions of satellites in `sat_orbit.comp` | the pixels of one close-up satellite |
| Normals | one per facet, merged | smooth normals on cylinders, cones, spheres and profiles (except across a crease of more than 35°) |
| Extras | none | UVs in metres (for the procedural patterns), the component index, the attitude group, a profile's inside (`inner_material`) |

Both share the rest pose and the body frame, so `evalGroupPoses()` poses the render mesh exactly as it poses the
lobes, and the occluders `buildSatOcclusion()` makes for the photometry line up with the drawn geometry.
`SatMeshRenderer` keeps every model's render mesh in one vertex and index buffer, with its materials, occluders
and per-component data (pivots, stow and fold state, build dates) in storage buffers.

**Stowed, folded and unbuilt parts** are handled in the vertex shader (`sat_mesh.vert`): a part with a `stow` is
scaled toward its root plane by the instance's unfolding progress, a part with a `fold` is turned about its hinge
line after that, and the normals follow by the inverse transpose. A part whose build date (`since`) is later than
the sim time, with the deployment era on, is not drawn. Satellites in the roster's slots are always fully
deployed; see [Satellite models](../modding/satellite-models.md#stowed-for-launch).

## Units and BRDF

The mesh shader (`shaders/sat_mesh.frag`) works in the sky pass's units: **pre-exposure π × radiance per unit
solar irradiance**, so a sunlit white Lambertian face reads albedo × cos, like terrain. Its BRDF is the
photometry's, per pixel:

- Lambert diffuse;
- GGX (or Beckmann, per material) × Schlick Fresnel × Smith shadowing (Walter's rational fit for Beckmann);
- the Sun's angular disc folded into the roughness, \(\alpha^2 \leftarrow \alpha^2 + 0.0023^2\) (`SUN_ALPHA`).

Because the units and BRDF match, summing a render's pixels reproduces the lobe model's radiant intensity; the
[photometric check](#the-photometric-check) measures exactly that.

## Lighting

| Light | Treatment |
|---|---|
| **Sun** | the instance's Sun direction × `litFactor` (the Earth's umbra and penumbra) × a reddening tint near the shadow edge; shadowed per pixel by rays against the model's own primitives |
| **Transmission** | light on the far side of a translucent blanket (ISS-style Kapton arrays) leaves this side diffusely, \(T \cdot E \cdot (-n\cdot s)_+\), tinted (amber for Kapton) and, with the solar-cell pattern, only through the gaps between cells |
| **Earthshine** | diffuse light from the whole lit Earth cap: an order-4 spherical-harmonic fit of the cap's irradiance on a plane, floored by the exact vector value, \(E(n) = \max(\text{SH}(n),\ E\,(n\cdot d)_+)\), the same table the photometry uses; or, with an environment probe, the probe's SH irradiance |
| **Moonlight** | diffuse, directional, × the Moon's light |
| **Local lights** | up to 8 point lights per frame: a burning engine (just behind its nozzles), and at night each floodlight of a launch pad (about 2·10⁴ cd, a 40° beam aimed at the pad's centre) and of a landing ship (45° at the deck); diffuse and specular, \(E = I/(r^2 + r_s^2)\), a floodlight soft over its beam's outer 10°, no shadow rays |
| **Reflections** | the reflected ray into the sharp-reflection G-buffer, else an environment probe, else the analytic `earth_env.glsl` fallback (see below) |

**Self-shadowing** follows the photometry's rule: **a primitive never shadows its own surface.** Every part is
convex or flat, so this is exact, and without it a faceted cylinder's surface would shadow itself. Open-lattice
parts (trusses) never shadow (their occluder kind is 0xFF), and neither does a part while it is stowed or folded,
since it is not where its shadow shape says. A profile shadows as the exact cone frustum through its end radii, a
fin as its bounding box. The ray test mirrors `rayHitsOccluder()` in `SatModel.cpp` and `sat_orbit.comp`.

**Local lights.** The lights are chosen on the CPU from the previous frame's launch visuals, the most important to
what is near the camera first (intensity over distance² + 50²), lifted at night and scaled by the eye's adaptation
to the engine lights exactly as the light they shed on clouds is (see [Launch visuals](launch-visuals.md)). A
landing booster, a pad's tower and a ship's deck are lit by them. They are not part of the photometric model, so
they seed no bloom.

**Engine bells.** Parts made of the material named `soot` (the inside of every engine bell in the rocket models)
glow while their stage burns: orange at the lip to white-hot up the bell, toward the throat as the view looks
into it. Each colour channel is capped at what displays as orange (the lip) to yellow-white (the throat) at the
frame's exposure, so the glow keeps its colour under the night exposure. The bells are render-only parts, so the
glow seeds no bloom or glare.

### Shadow-ray acceleration

Every shading pixel casts exact rays against the model's primitives: toward the Sun, toward the far side of a
translucent blanket, and along the reflected direction. Testing every primitive of a station per pixel, with its
pose composed per pixel, costs tens of milliseconds when a station fills the screen. So once a frame, per instance,
the CPU (`SatMeshRenderer::writeShadowAccel()`) poses every blocking primitive in world space with a bounding
sphere, and bins their shadows on a 16 × 16 grid on the plane across the Sun (64-bit masks, one bit per occluder:
`kMaxOccluders` is 64). A pixel's Sun ray tests only its cell's primitives, and every ray first rejects a primitive
by its sphere. The reflected ray also tests its facing first (a reflection that leaves below the surface is not
drawn, blocked or not). The image is the same as the brute-force loop.

The accelerated slots are the 64 scene instances largest on screen (radius over distance), the info window's
viewer and the photometric check; the rest keep the per-pixel loop. Measured on an RTX 3070 Ti at 1920 × 1009, a
close-up ISS costs about 1 ms of mesh pass; render-only detail adds only vertex work.
Knockout bit 8388608 (`mesh_shadow_accel_disabled`) restores the per-pixel loop for comparison.

**What the bloom counts.** Only sunlight, transmission and the photometric earthshine are part of the lobe
model. The pixel's alpha carries that **photometric share** of its light alongside the instance slot, and the
bloom and the mesh glare seed only that share. Reflections and moonlight are drawn but not counted: feeding them
to a bloom normalised by the model's intensity would make a satellite edge-on to the Sun (model intensity near
zero, reflections bright) explode into glow. Render-only components (`render_only` in the model file) have a share
of 0 on every pixel (`instCompRenderOnly()`, from a +100 flag on the component's parent index in the components
buffer): they are not in the photometric model at all, so a sunlit fitting on a satellite seen from behind its
arrays would otherwise seed a glow far brighter than the satellite itself.

### Procedural surface detail

A material's `pattern` (`SatSurfacePattern`: none, solar cells, MLI, panel seams, truss, heat-shield tiles; by
default chosen from the preset) adds visible structure that is **photometrically neutral**:

- each pattern scales the diffuse albedo by a factor whose **area-weighted mean is 1**, leaving the specular
  alone (the cover glass over cells is continuous);
- each is an exact box filter (`lineCover()`, `gridCover()`), so it keeps its mean at any distance and fades to
  the plain material when its features drop below a pixel;
- the features are large enough to see with the whole model in frame: 0.4 m array modules with gaps and a
  0.25° tilt per module, cells within them, MLI quilting seams and 12 cm crinkle;
- heat-shield tiles are 0.31 m hexagons in staggered rows with 6 mm gaps showing the lighter felt, each tile a
  little darker or lighter and about 0.6° off the surface, with 1.5 % pale replacement tiles; they fade to the
  plain material once a tile is under about 3 pixels.

The viewer's "Detail" button toggles the patterns.

### Open lattices

A material with `coverage` below 1 (the `truss` preset) is an open lattice. The photometry counts
coverage × area; the renderer cuts the members out (`latticeCover()`: a bay grid plus alternating diagonals,
member width solved on the CPU for the same area fraction) in the depth pre-pass. Each pixel is kept against a
threshold, so sub-pixel members at a distance become the right fraction of pixels, with the far side's inner faces
visible behind them:

- in the main view with the sky's temporal anti-aliasing on, the threshold is interleaved gradient noise stepped
  by the golden ratio every frame, so the TAA averages a sub-pixel lattice (a catch tower from a kilometre away) to
  its mean coverage, a fine translucent lattice rather than a fixed speckle that swims as the view moves;
- elsewhere (the viewer's MSAA, the TAA off) it is a fixed per-pixel hash.

A material named `gridfin` is a rocket's grid fin: a square grid of plates with no diagonals. Once a cell is
under 3 to 6 pixels it is drawn solid at the grid's mean look instead of cut out, since a sub-pixel grid of plates
reads as noise. The cut-outs are real geometry: the Detail toggle does not remove them.

## Which satellites get a mesh

Any model satellite whose mesh spans more than about 1.5-3 px on screen is drawn as a mesh, with no selection
needed.

```mermaid
flowchart LR
  O["sat_orbit.comp<br/>meshPx per satellite"] --> F["sat_flare.comp<br/>candidates past nominatePx<br/>(≤ 1024)"]
  F -->|next frame| C["recordMeshScene() (CPU)<br/>sort by size, take ≤ 256,<br/>poses + photometry in double"]
  C --> K["MeshKeepBuf<br/>sprite keep weights"] --> F2["sat_flare.comp<br/>(same frame)"]
  C --> R["SatMeshRenderer::recordScene()"]
```

1. **Size.** `sat_orbit.comp` writes each model satellite's on-screen diameter into the pre-photometry record
   (`meshPx`), from the type's mesh radius and the pixel angle (0 when meshes are off).
2. **Nomination.** `sat_flare.comp` lists every satellite past the CPU's nomination size into
   `GpuSatListHeader::meshCand[1024]` (satellite, size, final effectFlare, sprite size).
3. **Choice.** The CPU sorts last frame's candidates by size and meshes the largest 256
   (`kMaxMeshInstances`; one slot is kept for the followed satellite). There is one frame of lag in **which**
   satellites are meshed, none in **where** they are.
4. **Adaptive fade size.** `meshFadePx` (minimum 1.5 px) eases toward the size of the 80 %-of-the-cap'th
   largest candidate (rising in about 0.15 s, falling in about 1 s) but never below the cap'th, so no more than
   the cap ever fade in and none is cut off part-way through its fade. It rises by 30 % while the candidate
   list overflows. With a fixed threshold, in the AI ring about 250 satellites are mesh-sized at once, and
   which of them got a mesh would change every frame.
5. **Per instance**, evaluated in parallel on the CPU in double: the pose (`evalGroupPoses()`), the photometry
   (`evalSatPhotometry()`: lit factor, earthshine), the fades, the bloom scale, the glare normalisation and the
   glint direction.

**The followed satellite** is always meshed, decided on the CPU, even in the Earth's shadow where the GPU never
lists it.

### Launch objects

Rockets, the payload stacks on an upper stage, fairing halves, launch pads and landing ships are not in the GPU's
candidate list. The CPU places them itself, each as a fixed state (position, velocity and body axis, from the
launch model at the frame's time), in reserved instance slots: up to 6 rockets, 16 pads, 48 stacked satellites and
8 ships, each drawn once it is about 1.4 px across, plus a slot for the followed transient and one for the
selection. Rocket models' attitude comes from their body axis rather than an attitude law. See
[Rockets](../simulation/rockets.md) and [Launches](../simulation/launches.md).

### The hand-off from sprite to mesh

| | Range (× `meshFadePx`) | Default |
|---|---|---|
| mesh fades in | 1 → 2 | 1.5 → 3 px |
| sprite fades out | 1 → 6.7 | 1.5 → 10 px |

The sprite (the magnitude-based point, bloom and glare) fades over a wider range than the mesh fades in, so
the flare the observer expects stays with the satellite while its model resolves. The CPU writes the sprite
weights to `MeshKeepBuf` (`GpuMeshKeepList`), which `sat_flare.comp` reads **in the same frame**; if the GPU
faded the sprite the frame it nominated it while the mesh arrived a frame later, the satellite would blink out
for a frame.

While handing over, a meshed satellite's sprite is placed **on its glint**: at the Sun's image in its dominant
lobe, weighted by that lobe's specular share, rather than at the model's centre, so the flare does not jump
when the mesh's own glints take over. The image point (where the mirror direction from the camera meets the
lobe's plane, computed on the CPU in double) is clamped to the lobe's extent as a **square** of the lobe's area,
along the group's own in-plane body axes. A circle of half the side would cut off the corners: a 55 m Reflect
mirror seen a few degrees off its beam axis has the Sun's image about 35 m from its centre, toward a corner. It is drawn at 98 % of its range so its own mesh does not depth-hide it.

**Energy-matched bloom.** Each instance's `bloomScale` is the bloom seed its sprite would have put down,
\(b(\text{effectFlare}) \times\) the point disc area, per unit of rendered flux \(\sum L\,\Omega\,r^2/\pi = I\),
times the share the sprite has given up. Sprite bloom plus mesh bloom therefore stays constant as a satellite
resolves, and on a large model the glow sits on the glint. `glareNorm` (the sprite's effectFlare per unit of
seed) lets a mesh glint glare as the sprite did (see [Points, bloom and glare](points-bloom-glare.md#mesh-glints)).

**Only a compact image of the Sun glares.** The pixel's alpha also carries whether its surface may glare,
\(1 - \operatorname{smoothstep}(0.04, 0.10, \alpha)\) of its roughness (`meshGlareRoughK()`): mirrors, quartz
radiators, dielectric film and solar glass glare; stainless steel (0.12) and paint do not. A rough lobe spreads
the Sun over degrees, but at grazing incidence (Fresnel near 1, the masking term's \(1/2\alpha\)) it still reaches
glare levels, and every edge and panel seam of a sunlit Starship would flare. The viewer's glare source applies
the same gate. Mesh glints take part in the screen-space glare clustering like every other glare (see
[Points, bloom and glare](points-bloom-glare.md)).

**Picking.** Meshed satellites are hit-tested against their model's bounding circle (`meshDrawn`), ahead of
sprites, since they have no sprite left to click.

## Rendering the scene meshes

`SatMeshRenderer::recordScene()` runs first in `recordCompute()`, before `scene_depth.comp`, so that clouds and
beams stop at a mesh.

- **Positions** are camera-relative, from double, in ECEF axes with the origin at the eye. The camera rotation
  is ENU(observer) × `SkyCamera`, the same basis the sky uses.
- **Projection.** Infinite **reverse-Z**: depth = 2 cm / distance, compare GREATER, cleared to 0. Float depth
  then keeps precision from 1 cm to beyond 1000 km.
- **Two draws per mesh.**
    1. A depth pre-pass (`sat_mesh.frag -DMESH_DEPTH_PASS`): only the open-lattice discard.
    2. The shading pass (`-DMESH_SCENE_PASS`) with depth **EQUAL**, depth writes off and
       `early_fragment_tests`, so each covered pixel runs the shader once. With the discard in the shading
       pass early-Z would be off, and every overlapping layer of a close-up station would pay the full shader
       and its shadow rays.

!!! warning "Invariant"
    `sat_mesh.vert` declares `invariant gl_Position`. The depth pre-pass and the EQUAL shading pass are
    separate pipelines; without invariance their depths can differ in the last bit and the EQUAL test drops
    pixels.

- **Targets** (full swap extent, `VK_IMAGE_LAYOUT_GENERAL`, cleared every frame):

| Target | Format | Holds |
|---|---|---|
| radiance | RGBA32F | pre-exposure radiance × the mesh fade; alpha = instance slot + 1 + the photometric share |
| distance | R32F | **true** distance from the camera (0 = none) |
| reflection G-buffer | RGBA32UI | for mirror-smooth pixels: octahedral reflected direction, Fresnel × tint × fade weight (half floats), slot + 1 |

### How meshes join the frame

The mesh targets are read with `imageLoad`, because `sat_sky.frag` sits at the 16 sampled-image floor.

| Consumer | Reads | Effect |
|---|---|---|
| `scene_depth.comp`, half pass | the mesh distance, minimum over the covered full-res texels | the shared depth includes meshes, so clouds and beams stop at them |
| `sat_sky.frag` | radiance and distance | a nearer mesh **is** the pixel's surface: the atmosphere loop stops there and attenuates it, disc gates hide behind it, the cloud composite is skipped where the cloud is farther, and the unified depth includes it |
| `mesh_bloom.frag` (inside the flare-source pass) | radiance | each quarter-res texel sums its pixels' photometric light × `bloomScale` into the bloom seed, and their Sun-like light × `glareNorm` into the glint alpha |
| hardware depth | via the sky pass | a sprite at 98 % of its range stays in front of its own mesh |

Meshes are not drawn under the Potato tier (`sat_sky_minimal.frag` has no composite for them) or with knockout
bit 2097152.

## Reflections

A reflection takes the first available of three sources:

1. **Sharp reflections** (mirror-smooth pixels of up to four chosen instances);
2. an **environment probe** of the full renderer seen from the satellite;
3. **`earth_env.glsl`**, an analytic fallback: the Potato sky's closed-form atmosphere, the textured ground
   and a flat cloud deck, rewritten in ECEF from any origin and scaled into the scene's units, read at a mip
   matching the reflected footprint on the ground.

The Sun is in none of them: the GGX Sun lobe is the glint.

### Environment probes

`SatEnvProbes` renders `sat_sky.frag -DSKY_ENV` as the six faces of a cube around a satellite's position, in
pre-exposure HDR (see [Atmosphere and sky](atmosphere-and-sky.md#shader-variants) for what that variant draws).
A probe is:

- an RGBA16F cube, 256² per face for scene probes (0.35° per texel) and 512² for the model viewer's (0.18°);
  128² reads as pixelated in a mirror;
- a full box-filtered mip chain: `sat_mesh.frag` reads the mip whose texel spans about 2α of the lobe, with
  the texel size and the last useful level taken from the bound cube itself;
- an order-2 SH irradiance fit (`env_probe_sh.comp`: 1024 Fibonacci-sphere directions read from a blurred mip),
  used as the diffuse Earth light in place of the photometric earthshine.

Each probe has its own cube image and descriptor set (pipeline set 1, bound per draw), so no cube-array
feature is needed. A probe inside the atmosphere takes the observer's light-pollution dome for its Milky Way and
zodiacal light. Near the ground the `SKY_ENV` render draws no procedural city, so its ground takes the city
lights' close-up mean (a fraction of the night map until its own pixel footprint reaches the street layout's
range, as the main view's procedural cities average), and it ignores the data that belongs to the main observer's
frame (the exact elevation data near the observer, the launch sites' coast patches and pad grading), which
describes another place. There are 8 slots: slot 0 belongs to the model viewer, slots 1-7 are shared by the scene
instances.

**Ownership.** A probe belongs to the satellite it was made for and **follows it**: its owner keeps it however
far it drifts between refreshes. Another instance shares the nearest probe within max(20 km, 2 % of its
altitude), or adopts one nobody claimed this frame (instances are assigned largest first), else takes the least
recently used slot. Until its own probe exists an instance borrows the nearest rendered one within 1000 km; only
with none does it fall back to `earth_env.glsl`. Tying probes to positions instead would let a satellite at
high time warp outrun the reuse radius every frame and flicker between probe light and fallback light.

**Refresh** (`renderScheduledEnvProbes()`, one probe per frame, `kEnvRendersPerFrame`):

| Priority | Probe | Faces |
|---|---|---|
| 1 | new | all six |
| 2 | drifted past max(2 km, 0.2 % of altitude) from where it was made, the furthest first | two |
| 3 | older than 3 s (clouds drift, the Earth turns, settings change) | two |

A jump in sim time of more than max(30 min, 10 wall seconds of the current warp) since a probe was made puts it
first among the rendered and re-renders all six faces: a probe made by day would otherwise light a pad's tanks
pale blue-white for seconds after a jump to night.

A refresh renders two faces at a time, so a satellite crossing the drift threshold every few frames does not
pay for a whole cube each frame; its recorded position updates once all six faces are redone. The viewer's
probe is refreshed a face per frame (all six for a new satellite, two per frame while it is over 20 km from
where the cube was made).

Probes are off under Potato, with the mesh knockout, or with "Full-renderer reflections" off.

### Sharp mirror reflections

A probe texel is 0.35°: a membrane mirror filling the screen would magnify each texel over dozens of pixels.
For the mirror-smooth pixels of up to `kMaxSharpReflInstances` = 4 instances (those whose type has a
mirror-smooth material, largest on screen first):

1. The scene shading pass writes the **sharp share** \(1 - \operatorname{smoothstep}(0.004, 0.02, \alpha)\) of the
   reflection to the G-buffer instead of reading a probe: the `mirror` preset entirely, an OSR radiator about
   three quarters. The rest of the lobe still reads the probe.
2. `SatMeshRenderer::recordReflections()` draws `sat_sky.frag -DSKY_ENV -DSKY_REFL` once per instance, from the
   instance's own position, inside its screen rectangle, into an RGBA16F target. Each pixel discards unless it
   belongs to that instance and renders the sky along the stored reflected direction, × the stored weight.
3. `mesh_refl_add.comp` adds that into the mesh radiance over the union of the rectangles, rescaling the alpha's
   photometric share, since a reflection is not part of the model the bloom is normalised by.

This is exact for a flat mirror at any zoom, and costs a sky render of the mirror's screen area.

## The model viewer

The satellite info window and its pop-out 3D view show a real satellite (or a rocket stage) where it is now,
rendered offscreen by `recordModelViewer()` (in `recordCompute()`, whenever either window is open):

- **One target** serves both views, sized to whichever is larger; each view samples a centred sub-rectangle of
  its own aspect, so neither is stretched. 4 × MSAA where the format supports it, resolved into a
  swapchain-format image shown through a `UIImage` (a Clay custom element with its own descriptor set).
- **Position and pose** come from the CPU evaluator in ECEF; the model sits at its real distance from the
  camera.
- **Hand-overs.** When the viewed object hands over to what continues it (a stack to its upper stage at staging, an
  upper stage to its entry, a train member to its roster slot), the view switches model in the same frame,
  keeping the camera's direction and scaling its distance by the two models' sizes (`viewerSyncModel()`). A subject
  with no model (a re-entry fireball) keeps the last model, posed at the fireball's position.
- **Lighting.** "Live" uses the sim's Sun (including the Earth's shadow), the viewer's own probe (slot 0) for
  reflections and diffuse Earth light, and a background rendered by the same `SKY_ENV` pipeline at the viewer's
  camera (`SatEnvProbes::recordViewerBg()`, tonemapped by `sat_mesh_bg.frag`). "Studio" uses a fixed Sun at
  35° and the analytic `earth_env.glsl`.
- **Exposure** follows the sky's rule at the satellite: the same curve on the Sun's elevation in the
  satellite's local sky, so a lit satellite over the twilight Earth is shown under the night exposure, as in the
  main view.
- **Display-space terms** in the background and the slot-0 probe (stars, Milky Way, zodiacal light) are divided
  back by the viewer's exposure and gated by the viewer's Sun glare, so a mirror does not show a Milky Way the
  Sun has washed out.
- **Markers** (`sat_mesh_marker.frag`): a fullscreen pass after the model, depth-tested without depth writes.
  Each line pixel takes the line's own depth there, so the model hides the part of a line behind it; the dots
  are always visible unless the Earth hides them. Segments are clipped in clip space (w ≥ 10⁻³, |x|, |y| ≤ 1.5 w)
  before the divide, so a line passing near or behind the camera does not project to millions of pixels. The
  observer marker is the parked ground observer, never the camera.

### Glare on the viewer's glints

With "Glare" on (Live light, a tracked satellite), `SatMeshRenderer::recordViewerGlare()` draws the main view's
glare on the glints that make the flare the ground observer sees:

1. a half-resolution Sun-only render from the viewer camera, in shading **mode 3** (the check's shading,
   writing the **specular** sunlight as \(L d^2\));
2. `viewer_glare_find.comp` converts each texel to effectFlare (\(L d^2 \Omega/\pi\) × the observer's flare
   per unit intensity, including extinction) and finds 5 × 5 maxima as `glare_find.comp` does;
3. `glare_mesh.vert/.frag` draw them additively onto the resolved image.

Only reflections glare, so a Sun-facing rough array does not. The glints carry the viewer camera's range, so
they take the full proximity glare size. At the "Observer" camera preset the glints carry exactly their share
of the ground flare.

### The photometric check

"Check" renders the model Sun-only into a 512² R32F target, single-sampled, from the viewer's current direction
at 60 model radii (shading mode 1: scalar, no patterns, writing \(L d^2\)). The next frame reads it back and
integrates

\[
I = \sum_\text{pixels} L\,d^2\,\frac{\Omega_\text{px}}{\pi}
\]

which is the radiant intensity per unit irradiance. It is compared, as magnitudes at 1000 km, with
`evalSatLobesPosed()` for the same pose, Sun and direction. With shadows on the two differ by the lobe model's
occlusion sampling error; with shadows off, by render tessellation and smooth normals against baked lobes. The
accepted errors are listed in [Results and known residuals](../accuracy/results.md).

## Follow mode

In follow mode ("Go to") the camera rides with a satellite: the eye is satellite + an offset in its
along/cross/radial frame, in double, and the sky shaders receive it as the observer. The followed satellite is
always meshed (even in shadow), its sprite fade goes through the same keep list, and the scene projection's
reverse-Z keeps a mesh centimetres from the camera precise. The camera and flight logic belong to
[Controls](../using/controls.md).

## Settings

| UI label (tab) | `settings.json` key | Default |
|---|---|---|
| Full-renderer reflections (Photometry) | `photometry.full_renderer_reflections` | on |
| Sharp mirror reflections (Photometry) | `photometry.sharp_mirror_reflections` | on |
| Satellite part occlusion (Photometry) | `photometry.sat_part_occlusion` | off |

The mesh pass's knockout bit (2097152) and the shadow acceleration's (8388608) are profiling switches, not
settings.

## Where in the code

- `src/simulations/SatMeshRenderer.h/.cpp`: the renderer (`recordScene()`, `recordBloom()`,
  `recordReflections()`, `recordViewer()`, `recordViewerGlare()`, `recordCheck()`, `writeShadowAccel()`),
  `GpuMeshFrame` (with the local lights), `GpuMeshInstance`.
- `src/simulations/SatEnvProbes.h/.cpp`: probes and the viewer background.
- `src/simulations/SatMesh.h/.cpp`: `buildSatRenderMesh()`.
- `src/simulations/SatelliteSim.cpp`: `recordMeshScene()`, `renderScheduledEnvProbes()`,
  `nearestRenderedProbe()`, `envActive()`, `recordModelViewer()`, `viewerSyncModel()`;
  `src/simulations/SatelliteSimLaunches.cpp`: the launch objects' mesh jobs and `meshLightAdd()`; `src/simulations/SatelliteSimUI.cpp`:
  `updateViewerView()`, the info window and pop-out.
- `shaders/sat_mesh.vert/.frag`, `shaders/include/sat_mesh_common.glsl`, `shaders/include/earth_env.glsl`,
  `shaders/mesh_bloom.frag`, `shaders/mesh_refl_add.comp`, `shaders/env_probe_sh.comp`,
  `shaders/sat_mesh_bg.frag`, `shaders/sat_mesh_marker.frag`, `shaders/viewer_glare_find.comp`.
