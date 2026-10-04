# Rendering architecture

This set of documents describes how SatelliteSim forms an image: what each pass computes, how passes hand
data to each other, and the sampling, lighting and compositing techniques they use. It documents the code
as it is (October 2026, branch `1.2_dev`). For why a technique is shaped the way it is, see the dated notes
in `CLAUDE.md`. Where those notes have drifted from the code, the code wins; `FINDINGS.md` lists every
drift found while writing this.

| Document | Covers |
|---|---|
| **README.md** (this file) | The frame graph, resolutions, shared conventions (coordinates, depth, units), how the parts meet |
| [CLOUDS_FIELD.md](CLOUDS_FIELD.md) | The volumetric cloud density field: weather cube, noise volumes, cloud layers, Cb towers, fog/dust |
| [CLOUDS_MARCH.md](CLOUDS_MARCH.md) | The view ray march: sampling and jitter, stepping, lighting (light march, phases, ambient, optics), integration, fog/dust march, beam light and shafts |
| [CLOUDS_TEMPORAL.md](CLOUDS_TEMPORAL.md) | Rate modes, tile classification, temporal resolve, the far cloud layer, lightning, light volume, the `cloud_march.comp` post pass, ground shadows |
| [TERRAIN_AND_SURFACES.md](TERRAIN_AND_SURFACES.md) | The height function, the terrain march and its seed pyramid, the shared scene depth, land/sea/city shading |
| [SKY_AND_COMPOSITE.md](SKY_AND_COMPOSITE.md) | `sat_sky.frag` as compositor: atmosphere scattering, the cloud composite, Sun/Moon/stars, tonemap, sky TAA, auto exposure |
| [POINTS_AND_MESHES.md](POINTS_AND_MESHES.md) | Satellites as point sources and as meshes, the point PSF, bloom and glare, environment probes, occlusion of points |
| [FINDINGS.md](FINDINGS.md) | Possible bugs and stale documentation found while writing this set |

---

## 1. The image in one paragraph

The background is drawn by one large fragment shader, `sat_sky.frag`. It traces each pixel's ray against
the terrain (seeded by a lower-resolution depth pass) and the satellite meshes. It integrates single
scattering through the atmosphere up to the first surface, shades that surface, and composites the clouds
over it. The clouds come from a separate half-resolution compute pipeline that ray-marches a procedural
density field and accumulates it over time. Point sources (satellites, stars, planets, far city lights)
are drawn over the background as additive sprites, depth-tested against it. A quarter-resolution bloom
and full-resolution glare sprites go on top, then the UI.

```
           ┌───────── recordCompute ─────────┐                               ┌── main render pass ──┐
mesh scene ─► scene depth (¼ → ½) ─► sat_orbit ─► beam march ─► CLOUDS ─► sat_flare ─► bloom
 (full res)    terrain + mesh          (list)        (beams)     (½ res)   (photometry)  (¼ res)
                    │                                    │           │          │
                    └─────── sceneDepthImg (½, R32F) ────┴───────────┘          │
                                        ▼                                       ▼
                           sat_sky.frag (full res) ─► points / stars / planets ─► bloom composite ─► glare ─► UI
                           [optionally via sky TAA]
```

## 2. Frame graph

There is one command buffer and one frame in flight (`App::drawFrame`). The order is fixed:

### 2.1 `SatelliteSim::recordCompute`

| # | Pass | Writes | Resolution | Notes |
|---|---|---|---|---|
| 1 | `recordMeshScene` → `SatMeshRenderer::recordScene` | mesh radiance RGBA32F, true distance R32F, reflection G-buffer RGBA32UI | full | Depth pre-pass, then the shading pass with depth EQUAL. Env-probe cube faces (`SKY_ENV`) and sharp mirror reflections (`SKY_REFL` + `mesh_refl_add.comp`) are rendered here too. |
| 2 | `scene_depth.comp`, quarter pass | `sceneDepthQImg` R32F; `terrainFrameBuf` (eye ground height) | ¼ | Terrain from the eye |
| 3 | `scene_depth.comp`, half pass | `sceneDepthImg` R32F, linear metres | ½ | Seeded by the quarter pass; takes the min with the mesh distance |
| 4 | `sat_orbit.comp` | compact visible list, beam list, beam glow dome | per satellite | Orbit, attitude, reflectance. Appends visible satellites. |
| 5 | `beam_self_march.comp` | per-beam cloud block altitude and opacity | per beam | Marches the cloud field along each Reflect beam |
| 6 | `recordCloudsV2` | resolved cloud image + depth (½), far layer (full) | see CLOUDS_TEMPORAL | weather bake (one face) → far layer → lightning → light volume → tiles → march A/B → resolve → history copy |
| 7 | `cloud_march.comp` | `cloudMarchTargetA/B` RGBA16F | ½ | Composite/post pass: despeckle, lightning, rain drops, aurora, red airglow, beam lines, ground shadow |
| 8 | `sat_flare.comp` (indirect) | finished visible list | per visible satellite | Photometry: sky dimming, extinction, light pollution, sprite size, mesh hand-off |
| 9 | `city_sprites.comp` | appends to the visible list | lattice levels | Far city lights as point sources |
| 10 | flare-source render pass | `flareSourceImg` RGBA16F | ¼ | Sprite bloom seeds, the Sun's seed, mesh bloom + glint alpha |
| 11 | `glare_find.comp` | glint list | ¼ | Only when meshes were drawn |
| 12 | `flare_blur.comp` ×4 | `flareSourceImg` | ¼ | Narrow + wide Gaussian |
| 13 | trails (optional) | `trailAccumImg` | full | |
| 14 | `recordModelViewer` (optional) | viewer target | window | The satellite info window's 3D view |

### 2.2 `recordPrePass`, `recordDraw`, after the pass

- **Sky TAA path** (default at render scale 1): `sat_sky.frag -DSKY_TAA` renders the background offscreen
  with a sub-pixel jitter. `sky_taa.comp` resolves it against its history, and the result is blitted into
  the swapchain. The main pass opens by restoring the depth (`taa_depth_restore.frag`).
- **Render scale < 1**: the sky renders into a smaller target, which is blitted up into the swapchain.
- **Otherwise**: `sat_sky.frag` is the first draw of the main pass.
- **Main pass draw order**: background (or depth restore) → satellite points → stars → planets → bloom
  composite → glare sprites (satellites, then mesh glints) → trail composite → UI.
- **After the pass**: `recordScreenshotCopy` blits the centre of the presented image to 64×36 every frame
  while auto exposure is on (the meter), and does the screenshot copy when one is pending.

### 2.3 Timestamp buckets (Settings → Display → GPU frame breakdown)

| Bucket | Actually contains |
|---|---|
| Scene depth | mesh scene pass, env probes, sharp reflections, both scene-depth passes |
| Orbit compute | `sat_orbit`, `beam_self_march` |
| Cloud march | all of `recordCloudsV2` and `cloud_march.comp` |
| Flare compute | `sat_flare`, city sprites, bloom source/blur, glare find, trails, model viewer |
| Sky background draw | the sky shader only |
| Satellite + star draw | **also** the sky TAA resolve + blit (or the render-scale upscale), because that slot is written before them |
| UI overlay | UI |

## 3. Resolutions

Everything except the sky's own render target follows the **swap extent**. Render scale shrinks only the
sky pass.

| Size | Images |
|---|---|
| full | swapchain + depth, mesh scene targets, far cloud layer, sky TAA colour/depth/histories, trail accumulator |
| ½ = ⌈W/2⌉ | `sceneDepthImg`, all clouds-v2 screen images, `cloudMarchTargetA/B` |
| ¼ = ⌈W/4⌉ | `sceneDepthQImg`, `flareSourceImg`, `flareScratchImg` |
| quarter grid of ½ | the sparse cloud march's output (top-left quarter of the ½ images) |

The half-resolution depth and cloud targets have **identical dimensions on purpose**. `cloud_march.comp`
and the cloud passes read `sceneDepthImg` 1:1 with `texelFetch`, and `sat_sky.frag`'s joint-bilateral
upsample indexes both with the same texel coordinates.

## 4. Shared conventions

### 4.1 Coordinates and precision

- **Frames.** Most shaders work in the observer's ENU frame (x east, y north, z up) with the Earth's centre
  at the origin, so the eye is at `(0, 0, R_EARTH + h_eye)`. ECEF is used for anything fixed to the
  Earth, and the cloud map has its own *drifted* frame (§4.4).
- **No absolute ECEF position in float.** A float position on the Earth's surface resolves ~0.5 m, and
  noise read at such coordinates swims and stretches. Every procedural lattice (terrain detail, erosion,
  cloud noise volumes, city layouts, sea waves, rain drops) is **anchored at the observer**:
  - the CPU computes, in double, the observer's sea-level point as an integer lattice cell plus a fraction
    (`terrainAnchorCell/Rel`, `cv2.anchor*`, the sea's wrapped world offset);
  - the shader adds only a local offset to that.

  Lattice periods are powers of two, or divide the anchor cell, so nothing jumps when the observer crosses
  a cell.
- **Cancellation-free altitude.** Height above the sphere is computed as `(2R·q.z + |q|²)/(|p| + R)` from an
  observer-relative offset q (`tdAltitude`, `cv2Pos`). `raySphere` uses `c = (|ro| − r)(|ro| + r)`.
- **Never read a displacement, or anything multiplied up by a large factor, from an 8-bit
  hardware-filtered texture.** Hardware bilinear filtering has 8-bit sub-texel weights. So:
  - the cloud flow field is analytic;
  - Cb tower strength uses a float bilinear (`cv2WeatherBilinear`);
  - the DEM near the observer is filtered by hand (`tdDemBilinear`).
- **Never store a distance in a half-float.** The RGBA16F maximum is 65504. Distances live in R32F or RG32F
  images, or in RGBA16F alpha only as **kilometres** (the cloud occlusion alpha).
- **Thresholding linearly filtered coarse texels draws facets and straight creases.** Coarse fields that
  are thresholded are read C1-smooth: the fractional texel coordinate is smoothstep-remapped before one
  fetch (`cv2WeatherSmooth`, `cv2MesoSmoothG`).

### 4.2 Two depth representations

| | Where | Encoding | Written by | Read by |
|---|---|---|---|---|
| **Shared scene depth** | `sceneDepthImg`, ½ res R32F | linear metres along the view ray to the first terrain / sea / mesh surface; `kNoSurfaceT = 1e30` for sky | `scene_depth.comp` | the cloud march (clamps each ray), `beam_self_march`, `cloud_march.comp`, the sky pass (seeds its own march, joint-bilateral weights), flare-source/point manual tests |
| **Unified hardware depth** | main pass depth attachment | `log2(t/1 cm)/log2(1e9 m/1 cm)`, clamped to 0.99999, LESS, cleared to 1 (`include/depth.glsl`) | `sat_sky.frag` (`gl_FragDepth`) or the TAA depth restore | hardware depth test of points, stars, planets |

`sat_sky.frag` writes the unified depth from, in order:
1. its own full-resolution terrain hit, else the sea / far-land hit;
2. else the distance of an **opaque** cloud;
3. then the mesh distance, if nearer;
4. else, for a pixel on the Moon's disc, the Moon's distance.

Satellites test at their own range and stars and planets at infinity (`kDepthFar`). The point pipelines
do not write depth. So one rule holds everywhere: *a surface hides a point only if it is nearer than the
point*.

### 4.3 Radiance units

- **Sky, clouds, surface.** Physical-ish radiance with `SUN_INTENSITY = 1`, before exposure. Surfaces use
  terrain's convention: π × radiance per unit solar irradiance, so a sunlit white Lambertian face reads
  albedo × cos.
- **Exposure and tonemapping.** These happen once, at the end of `sat_sky.frag` (see SKY_AND_COMPOSITE).
- **Display-space terms.** The Milky Way, zodiacal light, Moon glow and the Sun's disc and glare are added
  after the tonemap, scaled by the global exposure.
- **Point sources.** These are in **effectFlare** units: `0.008` = magnitude 6, with satellites converted
  by `satFlareToMag`. The shared PSF model in `point_style.glsl` maps an apparent magnitude to a drawn
  Gaussian.

### 4.4 The cloud map drift

The weather (the 8K cloud map) rotates about the Earth's axis by `cloudDriftPhase()`, a pure function of
sim time plus a session offset. All cloud reads happen in that drifted frame (`cv2Drift`):
- the weather cube;
- the noise anchors, rotated on the CPU.

So the clouds move with the map as one body. The temporal resolve reprojects that motion exactly. Ground
height reads for clouds use the *undrifted* direction.

## 5. How the subsystems meet

| Producer | Consumer | Through |
|---|---|---|
| terrain (`scene_depth`) | clouds | `sceneDepthImg`: every cloud ray ends at the surface, with a soft contact fade over a few pixel footprints |
| meshes | terrain depth, clouds, beams, sky | mesh distance min'd into `sceneDepthImg`; `sat_sky.frag` reads the full-res mesh targets and treats a nearer mesh as the pixel's surface |
| terrain | the sky pass's own march | `sceneDepthImg` (½) seeds the full-res march (the ½ pass itself is seeded from ¼) |
| clouds | terrain and sea shading | `cloudMarchTargetB.a` = per-pixel sun transmittance through the cloud field to the surface point (the ground shadow) |
| clouds | sky composite | `cloudMarchTargetA` = cloud radiance + signed-km occlusion distance; `B.rgb` = transmittance |
| clouds | points | `cloudPointVisibilityAt`: per-texel test of the signed-km distance and transmittance against the point's range |
| clouds | Sun disc, lens flare, Sun bloom | `cloud.sunCloudT`: the eye's eased transmittance toward the Sun (lightning pass, thread 1) |
| clouds | rain / thunder audio | rain-rate map and flash list in the host-visible flash buffer |
| satellites (`sat_orbit`) | clouds | Reflect beams: `beam_self_march` marches the field per beam; the cloud march lights cloud with them (`beamLightCloud`) and draws the shafts; the sky pass draws the ground spots |
| satellites (`sat_flare`) | sky | 64-bin sky-glow histogram and the ocean-glint list |
| atmosphere | clouds | the cloud march takes **no** airlight of its own: it outputs attenuated cloud light, and the sky pass adds the air in front of the cloud (`airFront`), split at the cloud's mean distance |

## 6. Variants of the sky shader

`sat_sky.frag` is compiled several times, each variant with defines:

| SPIR-V | Defines | Use |
|---|---|---|
| `sat_sky.frag.spv` | — | main view without TAA, and the render-scale < 1 prepass |
| `sat_sky_taa.frag.spv` | `SKY_TAA` | main view with temporal AA (jittered, also writes an R32F depth copy) |
| `sat_sky_lite.frag.spv` | `SKY_LITE` | weak-hardware tier: cheaper atmosphere and fewer features |
| `sat_sky_env.frag.spv` | `SKY_ENV` | environment-probe faces from a satellite's position (no screen-space inputs) |
| `sat_sky_refl.frag.spv` | `SKY_ENV SKY_REFL` | sharp mirror reflections, per mesh instance |
| `sat_sky_minimal.frag` | (own file) | Potato tier: closed-form atmosphere, one flat cloud shell |
