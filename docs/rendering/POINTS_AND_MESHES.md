# Satellites, point sources and meshes

Satellites are drawn in one of two ways, and a satellite can be partly both during a hand-off:

- **as point sources**: a Gaussian sprite, plus bloom and glare, whose brightness is a physical apparent
  magnitude;
- **as meshes**, when large enough on screen: geometry models shaded with the same BRDF.

Stars, planets and far city lights use the same point model. This document covers how these are computed
and how they merge with the sky, terrain and clouds.

---

## 1. `sat_orbit.comp`: one thread per satellite

1. **Orbit.**
   - The orbital phase `u0 + n·Δt` is an exact two-float product (Dekker's algorithm, `precise`).
   - Δt arrives as a split double.
   - Positions are therefore metre-accurate after 7 days without a re-bake.
2. **Cull.** Satellites hidden by the Earth from the observer are dropped (limb-relative cutoff).
3. **Attitude.** Up to four attitude groups per type, built parents-first in fixed registers:
   - a root follows a two-vector (TRIAD) law or a tumble;
   - children hinge on their parent with a 1-DOF joint (track, edge-on, fixed, flare tilt).
4. **Ground-site mirrors.** The target is chosen per hashed lock window as a pure function of sim time,
   eased at a capped slew rate. They also emit a **beam record**: footprint, direction, intensity.
5. **Earth shadow.** Umbra and penumbra cones give `litFactor`.
6. **Reflectance.**
   - **Geometry models** (`modelFlux`): a sum over facet lobes of Lambert + GGX or Beckmann · Schlick ·
     Smith (the Sun's disc folded into α), plus diffuse transmission.
   - Lit by the Sun and by **earthshine**: the exact irradiance of the lit Earth cap, tabulated and fitted
     with order-4 spherical harmonics for diffuse light.
   - Optional per-part occlusion (sample points against the model's primitives; off by default).
   - Legacy types keep their tuned two-surface formula.
7. **Mesh size.** `meshPx` = the model's on-screen diameter.
8. **Append** to the **compact visible list**: one shared-memory count per workgroup, one global atomic,
   `atomicMax` for the indirect draw and dispatch counts. Slot order is nondeterministic, which is fine
   only because every point draw blends additively and writes no depth.

## 2. `sat_flare.comp`: photometry (indirect, per visible satellite)

| Step | Effect |
|---|---|
| Day / Moon sky | `effectFlare = flare / (1 + (dayBright·daySupp + moonBright·moonSupp)·atmFrac)`. `atmFrac` fades over 40-100 km of eye altitude. |
| Extinction | `atmExtinctionMag` (Chapman columns of an 8 km molecular and a 1.2 km aerosol component) along the line of sight to the satellite's range |
| Light pollution | the 16-sector dome, interpolated between sector centres, elevation falloff with an isotropic floor |
| Ceiling | a soft ceiling relative to the Sun's reference intensity |
| Mesh hand-off | a satellite past the nomination size is listed as a mesh candidate for the CPU (next frame). Its sprite is scaled by the CPU's keep weight, sits at its glint, and is drawn at 98% of its range so its own mesh does not hide it. |
| Sprite size | from the PSF model (§3) |
| Side outputs | the 64-bin sky-glow histogram (`atomicMax`) and the ocean-glint list |

`sat_flare.comp` does **no** occlusion. Terrain, cloud and Moon occlusion happen per fragment in each
consumer.

## 3. The point-source model (`include/point_style.glsl`)

One model maps an apparent magnitude, *after* every dimming term, to a drawn Gaussian. Satellites, stars,
planets and city lights all use it, so a mag-3 satellite looks like a mag-3 star.

```
D = 10^(−0.4·γ·(m − refMag)) − (same at limitMag)       // fades to exactly 0 at the limit
D ≤ 1 :  peak D, sigma σ0
D > 1 :  σ = min(σ0·√D, σmax),  peak D·σ0²/σ²            // flux-conserving widening
sprite edge = 2·(3σ + 1) px
```

The five parameters are the "Point ..." sliders. Satellites convert effectFlare with 0.008 = mag 6
(`satFlareToMag`); stars and planets convert relative flux (`relFluxToMag`).

## 4. Drawing points and occluding them

All point pipelines are additive (ONE/ONE), depth-test LESS, and **write no depth**.

| Source | Depth z | Cloud test | Terrain / mesh test |
|---|---|---|---|
| satellites (`sat_point`) | their range, in the unified log encoding | `cloudPointVisibilityAt`, per fragment, power 2 | hardware depth; the manual half-res test only for trails and render scale < 1 |
| stars, planets (`star_point`) | infinity (`kDepthFar`) | filtered cloud alpha / transmittance | hardware depth (any surface, cloud or Moon disc) |
| city sprites | their range × 0.995 | as satellites | as satellites |

Because the sky pass wrote the nearest of terrain, sea, opaque cloud, mesh and Moon disc into the depth
(SKY_AND_COMPOSITE §7):
- a satellite is hidden only by things nearer than it, and is drawn in front of the Moon;
- a star is hidden by everything.

**Cloud occlusion of a point** (`include/cloud_occlusion.glsl`). The cloud target's alpha is a **signed
distance in km**. For each of the four texels gathered around the point:
- an opaque cloud (alpha ≥ 0) nearer than the source hides it;
- a translucent cloud nearer than the source dims it by T^power, faded around the cloud's distance
  (±1% + 300 m).

The four results are blended with bilinear weights. Testing per texel, instead of filtering the alpha
first, avoids mixing a cloud's distance with the no-cloud sentinel at outlines.

## 5. Bloom and glare

**Bloom** (quarter resolution):

1. **Flare-source pass** (`flare_source`).
   - Each satellite's seed is `Gaussian · clamp(log2(effectFlare)/2, 0, 4)`, tested per fragment against
     cloud (power 1) and the half-res scene depth.
   - The Sun adds its own seed, × `sunCloudT` and the eclipse.
   - Then `mesh_bloom.frag` adds the meshes' over-white light (§7.3).
2. **`flare_blur.comp`.** A narrow separable Gaussian, then a wide one (σ 10 texels, "Flare streak"),
   ping-ponged into `flareSourceImg`.
3. **`flare_composite.frag`.** Additive, with a soft knee on luminance (unchanged below 0.45, approaching
   0.8). A day/night eye-adaptation gain scales it.

**Glare** (full resolution, `include/glare.glsl`).

- **What it is.** A sprite past "Glare threshold" in the bloom's log response:
  - a glint core;
  - a tight halo;
  - "Glare spikes" spikes of constant pixel width, fading as (1 − r/len)^falloff, windowed to zero at the
    sprite edge.
- **Satellite glare** (`glare.vert/.frag`) is tested once, at the source's screen position, for cloud and
  terrain.
- **Proximity.** The radius grows toward "Glare near gain" within "Glare near range" of the camera, and is
  exactly 1× beyond it. A resolved mesh metres away glares wider than a point hundreds of km off.
- **Mesh glints** (`glare_find.comp` + `glare_mesh.*`):
  - **Which light may glare.** Only Sun-like surface radiance (π·L/E 150 → 1500) is written to the flare
    source's **alpha**: the Sun in a mirror, an OSR radiator. Satellite sprites write alpha 0.
  - **Finding glints.** `glare_find.comp` lists 5×5 local maxima past the threshold (≤ 64). They are drawn
    with the same profile.
  - **Point-like meshes.** While the mesh is still point-like (below 12 px, gone by 36), its glare stays
    the **sprite's**, at the glint position, so it does not jump between texels.

## 6. City sprites

`city_sprites.comp` runs after `sat_flare` and appends **finished** records to the same visible list. It
walks a world-fixed lattice whose cell grows with distance (7 levels, out to ~1000 km). A cell holds a light
with probability ∝ the night map's density, for points:

- a few metres above the DEM;
- above the sea-level horizon;
- within the city terrain limit;
- only where a pixel spans more than "City sprites from" of ground.

Their brightness is the cell's area / r² × the slant air × scintillation, capped below glare. They are drawn
by the same point, bloom and glare draws. Picking skips them (index sentinel), and so does the trail pass
(`meshPx = −1`).

## 7. Meshes

### 7.1 Choosing what to mesh (CPU, `recordMeshScene`)

- **Candidates.** `sat_flare.comp` lists candidates past the nomination size. The CPU sorts last frame's
  list by size and meshes the largest 256.
- **Adaptive fade-in size.** `meshFadePx` eases toward the size of the 80%-of-cap'th largest candidate, so
  at most the cap fade in and none is cut off mid-fade.
- **Per instance, in parallel, in double:**
  - pose (`evalGroupPoses`) and photometry (`evalSatPhotometry`);
  - mesh fade (1.5-3 px by default) and sprite fade (to 6.7× the fade size);
  - bloom scale, glare normalisation, the glint direction.
- **Keep buffer.** The sprite weights go to `sat_flare.comp` the **same frame** (`MeshKeepBuf`), so there is
  no "flare, nothing, flare" gap.
- **Followed satellite.** Always meshed.

### 7.2 Rendering (`SatMeshRenderer::recordScene`, before the scene depth)

- **Positions.** Camera-relative ECEF from double. Projection is infinite **reverse-Z** (depth = 2 cm /
  distance, GREATER, clear 0), precise from 1 cm to 1000 km.
- **Two passes per mesh.**
  1. A depth pre-pass (only the open-lattice discard).
  2. The shading pass with depth **EQUAL** and early fragment tests, so overlapping station layers do not
     each run the full shader. `gl_Position` is `invariant`.
- **Targets** (full res, cleared every frame):
  - RGBA32F pre-exposure radiance; alpha = instance slot + 1 + the photometric share;
  - R32F **true distance**;
  - RGBA32UI reflection G-buffer.
- **Shading** in the photometry's units and BRDF:
  - sunlight with per-pixel self-shadow rays against the model's primitives;
  - transmission through translucent arrays;
  - earthshine diffuse from the SH fit, or the environment probe's SH;
  - moonlight;
  - reflections, in priority: the sharp G-buffer → an environment probe mip → the analytic `earthEnv`
    fallback;
  - procedural surface patterns (solar cells, MLI, seams) that keep their mean.
- **Environment probes.**
  - Six faces of `sat_sky.frag -DSKY_ENV`, rendered from a satellite's position. RGBA16F cubes (256²; the
    viewer's 512²), a box mip chain, order-2 SH (`env_probe_sh.comp`).
  - A probe belongs to the satellite it was made for and follows it. At most one probe is (re)rendered per
    frame, two faces at a time.
  - Instances without a probe borrow the nearest rendered one.
- **Sharp mirror reflections.** For mirror-smooth pixels of up to 4 instances,
  `sat_sky.frag -DSKY_ENV -DSKY_REFL` renders the sky along each pixel's reflected direction, from the
  instance's own position, inside its screen rectangle. `mesh_refl_add.comp` adds that into the mesh
  radiance. This is exact for a flat mirror at any zoom.

### 7.3 How meshes join the rest of the frame

| Consumer | Reads | Effect |
|---|---|---|
| `scene_depth.comp` (½ pass) | mesh distance, min over the covered full-res texels | the shared depth includes meshes, so **clouds and beams stop at them** |
| `sat_sky.frag` | mesh radiance + distance (`imageLoad`) | a nearer mesh is the pixel's surface: the atmosphere loop stops there and attenuates it; disc gates hide behind it; the cloud composite is skipped where the cloud is farther; `gl_FragDepth` includes it |
| `mesh_bloom.frag` | mesh radiance | over-white energy seeds the bloom; Sun-like texels seed glint alpha |
| hardware depth | via the sky pass | a satellite's own sprite is drawn at 98% of its range, so it stays in front of its mesh |

**Energy matching across the hand-off.** Each instance's `bloomScale` is the bloom seed its sprite would
have put down, per unit of rendered flux, × the share the sprite has given up. Sprite bloom + mesh bloom
therefore stays constant as a satellite resolves, and on a large model the glow sits on the glint.
`glareNorm` (effectFlare per unit of seed) lets a glint glare like the sprite did.

Picking tests meshed satellites against their model's bounding circle, ahead of sprites.
