# Clouds: rate, temporal resolve, far layer and the post pass

A single march of every half-res pixel per frame is too expensive at most views, and a single march is
noisy (jittered starts, sub-texel positions, short light marches). The cloud pipeline therefore marches a
subset of pixels each frame and accumulates them in a reprojected history. This document covers:

- how the rate is chosen;
- the resolve;
- the far cloud layer that replaces the march from far orbit;
- the auxiliary passes (lightning, light volume, beam occlusion);
- `cloud_march.comp`, which turns the resolved clouds into what the rest of the frame reads.

Orchestration is `recordCloudsV2` and `fillCloudsV2Params` (`src/simulations/SatelliteSimCloudsV2.cpp`).

---

## 1. Pass order inside the cloud bucket

```
fillCloudsV2Params                       CPU: UBO, anchors, rate decision, reprojection inputs (double)
recordWeatherEvolution                   one cube face re-baked with all mips, when due
cloud_v2_far.comp        [if farBlend>0]  full-res far layer
   └─ farBlend ≥ 0.999 → stop here (no march, no resolve; history invalidated)
cloud_v2_lightning.comp  (5 workgroups)  flash list, eye rain map, eye Sun march + profile
cloud_v2_lightvol.comp   [god rays]      4 of 32 levels of the light volume
cloud_v2_tiles.comp      [adaptive]      list 32×32 tiles that need full rate
cloud_v2_march.comp  pass A              sparse / full / half grid
cloud_v2_march.comp  pass B [adaptive]   indirect, listed tiles at full rate
cloud_v2_resolve.comp                    reproject + clamp + blend
copy resolved → history
cloud_march.comp                         post pass → cloudMarchTargetA/B
```

All clouds-v2 screen images are created in `VK_IMAGE_LAYOUT_GENERAL` and stay there; only memory barriers
are used between passes. `cloudMarchTargetA/B` are the exception: they move between GENERAL and
SHADER_READ_ONLY each frame for `sat_sky.frag`.

| Image | Format | Size |
|---|---|---|
| new samples | RGBA16F + RG32F depth | ½ (sparse uses the quarter top-left) |
| full-rate (pass B) | RGBA16F + RG32F | ½ |
| resolved | RGBA16F + RG32F depth (read back in place as depth history) | ½ |
| history | RGBA16F | ½ |
| far layer | RGBA16F | full |
| light volume | R16F 128×128×32, 3D | — |
| weather cube | RGBA8 1024²×6, 8 mips | — |

## 2. Choosing the rate (CPU, each frame)

**History validity.** History is valid when:
- it was valid last frame;
- the cloud settings hash is unchanged;
- the eye moved less than 20 km;
- sim time moved less than 600 s.

Cuts, shader reloads, target recreation and far-layer takeover invalidate it.

**Still view.** The view is still when all of these hold:
- no camera rotation beyond 0.3 of a half-res pixel;
- eye motion ≤ 1 m;
- sim time step ≤ 0.5 s;
- no zoom change;
- valid history.

**Decision, in order.**
1. **Full rate** when the eye is above `full_rate_above_km` (30 km). From orbit, motion invalidates history
   and the sparse fallback looks pixelated. *Except*: with "sparse when still" on, after 8 still frames
   the march drops back to sparse, and the history converges on the same image.
2. **Half rate** (a checkerboard; off by default) replaces full rate when its toggle is on.
3. **Adaptive** replaces full rate when all of these hold:
   - "Adaptive rate" is on (the default);
   - the history is valid;
   - the eye moved ≤ 150 m this frame;
   - the workgroup is 16×16.

   The grid is then sparse except for tiles that need more (§3).
4. Otherwise **sparse**.

**Fast-flight LOD.** When boost is held and the eye moves (or it moves more than 150 m a frame):
- ≤ 2 light steps;
- 1.5× base step;
- 60% of the iteration budget.

**Forced full rate.** HQ photos and cinematic exports force full rate (`full_rate_above_km = 0`,
`sparse_when_still = 0`).

## 3. Adaptive tiles (`cloud_v2_tiles.comp`)

One 16×16 workgroup per 32×32 half-res tile, with each thread owning one 2×2 block's sparse pixel. A tile
votes for full rate when either:

- **parallax**: the clouds' motion this frame exceeds "Adaptive parallax" (1 px). The motion is
  `(|eye move| + volume slide) / (last frame's resolved depth × pixel angle)`.
- **off-screen history**: a pan uncovered it, i.e. the rotation-only reprojection falls off screen. There
  is no margin on this test: a pan uncovers about 2 px a frame, and any margin would mean no edge tile
  ever votes.

Voting tiles are appended to `TileBuf` with indirect args (4 workgroups each) and flagged. Pass A returns on
a flagged workgroup; pass B marches the listed tiles.

Why two passes: inside a full-rate dispatch, skipping pixels saves nothing, because a warp costs its
slowest lane. Why the vote is its own pass: inside the march it pushed the shader past the 128-register
allocation.

In a still view, nothing passes the threshold and the cost is the sparse grid. In fast parallax (flying
through a storm), only the tiles with near cloud pay full rate.

## 4. The resolve (`cloud_v2_resolve.comp`)

One thread per half-res pixel.

### 4.1 Fresh samples and the spatial estimate

**Fresh pixels.** A pixel is *fresh* if it was marched this frame:
- every pixel at full rate;
- this frame's parity at half rate;
- the block's one pixel at sparse rate;
- every pixel of a listed adaptive tile, read from the full-rate images.

**Neighbourhood statistics.** Built over the 3×3 of this frame's new samples:
- min, max, mean and standard deviation in **mean-colour space**, `(rgb/max(T, 0.02), T)`;
- min and max in plain radiance;
- the nearest cloud distance `dNear`;
- the opacity-weighted mean distance;
- the sum of the axial neighbours.

**Spatial estimate (for pixels with no fresh sample).**

| Rate | Fresh pixel | Other pixels |
|---|---|---|
| sparse | `mix(own, axial mean, 0.5)` | bilinear read of the quarter grid |
| half | `0.5·own + 0.125·diagonals` | mean of the four axial neighbours (fresh) |
| full | own + a 3×3 tent `blur` | — |

### 4.2 Reprojection

All reprojection inputs come from the CPU in double precision: the previous view, the previous eye, the
eye's ECEF delta, and the change in the map's drift.

1. Take the pixel's view direction in ECEF.
2. Pick a depth: its own resolved depth, else `dNear` (closest-cloud dilation for cloudless pixels), else
   infinity. A cloudless pixel reprojected at infinity smears small clouds into streaks in motion.
3. The world point is rotated by the drift change (the clouds move with the map), offset by the eye's
   motion and the shape wind's shift, then projected into the previous view.

This is exact for a rotation about a known eye. The history is sampled with a 9-tap Catmull-Rom.

### 4.3 Motion, clamping, blending

**Motion.**
- `motionPx` is the **parallax**: the reprojection at the cloud's depth versus the same ray at infinity,
  in pixels.
- It has two floors: the volumes' own slide, and the eye's sideways motion at the pixel's depth (capped at
  50 km).
- Pure rotation reprojects exactly, so it is not motion. Raising the blend weight during a pan swapped the
  accumulated image for the quarter grid's noisy samples.

**Weights.**
- `wNew = mix(still weight, moving weight, smoothstep(0.05, 1, motionPx))`.
- Still weight: "History weight", 0.05, capped at 0.15; it falls to 40% between 300 and 3000 km of
  altitude, against far-orbit flicker.
- Moving weight: "History weight moving", 0.4, capped at 0.5.

**Clamp box.**
- The clamp box widens for a still view: a tight box against the quarter-grid neighbours makes blocky 2×2
  squares in far cloud.
- In motion it tightens toward mean ± 1.25 sd (variance clipping).
- History is clamped in **mean-colour space**, then again in **plain radiance**. The mean-colour space
  divides by opacity, which inflates near-transparent pixels (inside beam shafts) by up to 50×. Clamped
  only there, cloud-edge history blew up into bright blocks.

**Block convergence.**
- As motion rises, a fresh pixel leans toward its spatial estimate, and the three non-fresh pixels of a
  sparse block take its weight. So all four pixels of a 2×2 block update nearly alike.
- In fast parallax (6-24 px), fresh samples blend toward this frame's 3×3 tent.

**Without valid history.** The output is this frame's spatial reconstruction.

### 4.4 The resolved depth

- **Value.** The resolved distance is the 3×3 new samples' opacity-weighted mean distance, not one ray's
  noisy estimate.
- **Still blending.** In a still view (parallax < 0.5 px **and** total texel shift < 0.5 px, rotation
  included) it blends at 0.1 with the previous value, read back in place.
- **Why depth history matters.** A sparse block's single sample moves on the four-frame cycle. The sky
  pass splits the air in front of clouds at this distance, so a jittering depth flickers that air.
- **Why the gate includes rotation.** Gating on parallax alone let a pitching camera blend the depth of a
  different direction into each texel, which drew streaks along the horizon.

## 5. The far cloud layer (`cloud_v2_far.comp`)

From far orbit a pixel spans kilometres, and the march's sub-texel sampling flickers. The far layer replaces
the march with a **2D** evaluation of the same placement.

- **When.** `farBlend` = smoothstep of the eye altitude between "Far cloud layer from" (600 km) and "full
  at" (8900 km). Above 0.999, the march, resolve and lightning are not dispatched.
- **Per full-res pixel.** The ray meets a sphere 1.5 km up.
  - `cv2FarColumn` is a hand-kept 2D copy of the low layer's placement: same flowed weather, fraction and
    morphology. It returns the strength `e`, the cover fraction and the type.
  - **Optical depth**: the type's extinction × a thickness from `e` × a density gain.
  - **Relief**: two more columns a footprint away give `e`'s gradient, which tilts the shading.
  - **Slant coverage**: `1 − (1 − f)^(1 + k·tan z)`.
  - A coverage bias in field units matches the march's 3D lobes, which put cloud slightly outside the 2D
    edge.
- **Lighting.** A conservative two-stream slab (g 0.85):
  - reflectance `R = aτ/(1 + aτ)`, with `a = 0.75(1 − g)`;
  - a direct-beam form `slabRmu` at the Sun's incidence;
  - the key light × μ₀^(1/(1 + "low-Sun light")), which keeps 3D tops lit near the terminator;
  - sky, Moon, night-sky and city light;
  - the eclipse shadow.
- **Mid and high layers.** Three samples each inside their lenses, lit as slabs and combined over the low
  layer by the **adding method**:
  `L = L_U + L_low·(1 − R_U)²/(1 − R_U·R_low)`.
  A plain attenuator drew grey camouflage over the decks.
- **Output.** RGBA16F: radiance, transmittance.
- **What it omits.** No scene-depth test (a half-res depth read per full-res pixel drew a dot grid), no Cb
  towers, no rain, no ground shadows.
- **Composite.** `cloud_march.comp` fades the march's clouds out by `farBlend`. `sat_sky.frag` puts the far
  layer *behind* the half-res composite ([SKY_AND_COMPOSITE.md](SKY_AND_COMPOSITE.md) §2).

## 6. `cloud_march.comp`: the post pass

This half-res compute pass (16×16) produces the two targets every later consumer reads. Despite the name it
no longer marches the main clouds. In order:

1. **Ray, ENU, the eye's detailed ground**, and the beam pointing-ray tile cull. The cull runs before the
   bounds check, for barrier uniformity.
2. **Legacy cirrus** (`cirrusMarchCS`): only when the v2 high layer is off or the march is knocked out.
3. **Read the resolved cloud** (radiance, T) and its depth.
4. **Despeckle.** A cloud texel with few neighbours of comparable opacity, or one much more opaque than its
   neighbours, is pulled toward its 3×3 tent. These isolated texels flickered along edges.
5. **Depth fill.** A cloudy texel with no distance takes its neighbours' opacity-weighted distance.
6. **Far blend.** Radiance × (1 − farBlend); T → 1.
7. **Lightning** (`lightningCS`), drawn **after** the resolve because a flash lasts a few frames and the
   history would swallow it.
   - **Glow**: in-cloud diffusion (~5 km) then the inverse square, × the resolved cloud's opacity, at its
     mean depth.
   - **Ground strokes**: a fractal bolt tree (main channel, branches, twigs), each segment a capped-peak
     core plus halo, dimmed smoothly where it passes behind cloud.
   - **Red sprites** (`spriteCS`), occasional.
8. **Rain, sleet and snow at the eye** (`rainDrops`).
   - Drops are **in the world**: up to 9 depth layers (2 m … "Drop distance") of a 3D lattice in a frame
     fixed to a 0.25° ground point.
   - The lattice falls at ~10 m/s (snow ~1.1) and drifts with the integrated gusting wind.
   - Each layer's drop probability is the rain rate read from the lightning pass's 32×32 rain map at that
     layer's point, so a shaft's drops reach the far layers first.
   - Drop type comes from the air temperature at the eye.
   - Ice crystals and backlit drops glint. With no rain, ice fog in sunshine draws diamond dust.
9. **Aurora** (`auroraMarchCS`) and **red airglow** (`airglowRedMarchCS`): additive, half-res, inside the
   same radiance channel.
10. **Beam pointing rays.** Each beam's visible streak is the closest approach between the view ray and the
    beam segment. It is cut by:
    - the scene depth;
    - the beam's own cloud block altitude (from `beam_self_march`);
    - a cloud gate past the view ray's opaque cloud.
11. **Ground shadow** (`cloudGroundShadowV2`, surface pixels only).
    - **Start**: 10 m above the surface point, toward the Sun. Past the terminator, the ray is lifted to
      the horizon.
    - **Low stretch**: up to 3.5 km along the *curved* ray, 20-60 steps of ≤ 2.5 km (long at a low Sun).
    - **Upper stretch**: 12 more steps through the rest.
    - **Field reads**: at the step's footprint (60-3000 m). The weather is read at coarser mips as the Sun
      drops (`gCv2WxLod`), so a low Sun does not draw the source JPEG's blocks.
    - **Rain**: curtains count at 15%.
    - **Jitter**: a 2×2 ordered start offset, which the sky pass's [1 2 1] tent averages out exactly.
12. **Write the targets.**

| Target | rgb | a |
|---|---|---|
| `cloudMarchTargetA` | **radiance** (clouds, cirrus, aurora, red airglow, lightning, rain, beam lines and shafts) | **signed occlusion distance in km**: ≥ 0 = opaque cloud, at its mean distance (`tHalf` if no mean); < 0 = −(mean distance of translucent cloud); −60000 = none |
| `cloudMarchTargetB` | **transmittance** (per channel) | **ground shadow**: Sun transmittance through the cloud to the surface point |

Naming trap: inside `cloud_march.comp` the transmittance accumulator is called `A_*` and is stored in target
**B**; the radiance `B_*` is stored in target **A**.

The alpha is in kilometres because RGBA16F overflows at 65.5 km in metres.

## 7. Auxiliary passes

### 7.1 Lightning (`cloud_v2_lightning.comp`, 5 workgroups)

**Workgroup 0: the flash list.**
- Walks the Cb tower lattice out to the horizon of 12-km tops, using the same lattice, roles and fill as
  the field.
- For each **dominant** tower it evaluates a flash schedule hashed from the cell and a 2 s slot of sim
  time:
  - a start in the slot and a duration of 0.25-0.85 s;
  - 1-4 return strokes over a fading glow;
  - a quarter of the strongest towers' flashes are cloud-to-ground;
  - an occasional red sprite.
- Deterministic and reversible in sim time. The output goes to `cv2FlashBuf` (host-visible); the host reads
  it for thunder.

**Thread 1: the eye's Sun march.**
- 32 quadratic steps toward the Sun, up to 400 km below the shell top.
- It writes:
  - the eye's cloud transmittance toward the Sun. The host eases it into `CloudParams.sunCloudT`, which dims
    the Sun disc, glare, corona, lens flare, the Sun's bloom seed, and the near-Sun direct in-scatter;
  - the 17-entry optical-depth **profile** `cv2SunProf` the march reads (CLOUDS_MARCH §4.2);
  - the ice-fog density at the eye.

**Workgroups 1-4: the eye's rain map.** A 32×32 map of 40 m cells around the eye. Each cell runs `cv2Field`
for rain × density. The drops read it, and the host reads its centre for the rain ambience.

### 7.2 Light volume (`cloud_v2_lightvol.comp`, god rays only)

- A camera-centred 128×128×32 R16F volume of Sun transmittance.
- Gnomonic columns over ±400 km, from sea level to 16 km.
- 4 levels re-baked per frame, each voxel 16 growing steps toward the Sun.

### 7.3 Beam occlusion (`beam_self_march.comp`, before the cloud bucket)

- One thread per Reflect beam.
- It marches the field (`cv2Field`, mean erosion, 60 m footprint) along the segment from the satellite to
  the beam's **actual** ground intersection: ~200 m steps, up to 96.
- It writes back into the beam record:
  - `blockAltM`: the altitude where T < 0.5, else the absorption-weighted mean altitude;
  - `blockOpacity` = 1 − T.
- The sky pass's ground spot and the pointing ray's height cut read these.
