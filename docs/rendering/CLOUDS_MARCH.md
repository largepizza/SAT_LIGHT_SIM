# Clouds: the view march, sampling and lighting

`shaders/cloud_v2_march.comp` ray-marches the density field ([CLOUDS_FIELD.md](CLOUDS_FIELD.md)) at half
resolution. It writes, per marched pixel, cloud radiance, transmittance and a depth pair. Which pixels are
marched each frame, and how the results are accumulated over time, is in
[CLOUDS_TEMPORAL.md](CLOUDS_TEMPORAL.md).

---

## 1. Dispatch and the pixel a thread marches

The workgroup is 16×16 by default. It is a specialization constant, because the group size bounds the
registers available per thread. The rate mode is `cv2.misc.z`:

| Mode | Pixels marched per frame | Output location |
|---|---|---|
| **sparse** (0) | one pixel per 2×2 half-res block, cycling (0,0),(1,1),(1,0),(0,1) over four frames | the quarter-size top-left of the targets |
| **full** (1) | every half-res pixel | in place |
| **half** (2) | a checkerboard, alternating each frame | in place |
| **adaptive** (3) | pass A: sparse grid except tiles listed for full rate; pass B (same SPIR-V, spec constant 2 = 1, indirect dispatch, 4 workgroups per 32×32 tile): full rate on listed tiles | pass B writes full-rate images (bindings 17/18) **and** that tile's sparse-grid samples, so the quarter grid stays complete |

A *visit* is one march of a given pixel. Every jitter sequence advances per visit, not per frame. Stepping a
sparse pixel by the golden ratio every frame would give each pixel of a 2×2 block its own cluster of
offsets, which shows as 2×2 squares in far cloud.

### 1.1 Jitter

| What | Source | Advance |
|---|---|---|
| **Sub-texel ray position** | R2 sequence `fract(ign(pix) + visit·(0.7549, 0.5698))`. Off (texel centre) while the eye moves more than 1 m a frame. | per visit |
| **View-march start** | 64×64 void-and-cluster blue-noise tile (SSBO binding 15) + golden step 0.618 | per visit, or per frame in motion |
| **Fine-lattice restart** | `fract(1.618·jitter + 0.5)`, derived from the start jitter. A second blue-noise read kept live across the loop costs the register budget. | — |
| **Light march** | another cell of the blue-noise tile (+32, +21), its own step 0.7549 | per visit |

The light march has its own jitter because it must not move with the view march. With a shared jitter, a
sample's depth inside a flat cloud top and its light-step positions move together, the product never
averages out, and contour rings form.

### 1.2 Tile cull of beam lights

Before the bounds check (so the barriers stay in uniform control flow), the workgroup culls the CPU's
Reflect-beam light list against its tile's view cone (`cullCloudLightsForTile`,
`include/beam_cloud_lights.glsl`). The result is a shared list of up to 128 lights. On overflow, or with
knockout bit 131072, every sample walks the whole list instead.

---

## 2. Ray setup

- **Eye and frame.** The eye is at `R_EARTH + terrainFrame.x + 2 m`, where `terrainFrame.x` is the
  detailed ground under the observer written by `scene_depth.comp`. The frame is the observer's ENU.
- **Scene depth.** `tScene = sceneDepthImg[pix]`, 1:1 at half resolution.
- **Shell.** `cv2ShellSegments` intersects the ray with the shell between the lowest base and the highest
  top, minus the inner sphere, giving 0-2 segments. No below / inside / above cases are needed.
- **End of each segment.** `min(segment end, tScene, tLimit)`. `tLimit` is the "max distance" (600 km)
  measured from the **shell entry**: from orbit the Earth is farther than that from the eye.
- **Soft ground contact.**
  `σ *= smoothstep(0, clamp(3·footprint, 40 m, 600 m), tScene − t)`. Without it, dense cloud meeting a
  slope ends on a hard stair-stepped line.

## 3. Stepping

```
tr    = t − shell entry
stepF = min(base + growth·tr,  max(cap, 0.012·tr))            // 60 m, 0.01, 1500 m by default
stepF = min(stepF, 200 m / max(|dir·up_at_sample|, 0.01))     // ≤ 200 m of altitude per step
```

**Coarse mode.**
- Each segment starts in coarse mode, after a start offset of `2·step·jitter` (one coarse interval), so
  across visits the coarse lattice takes every phase. Spanning only one fine step left thin decks found at
  some phases and skipped at others: rings around the nadir.
- The field is read with `detailAmt = −1` (no erosion), which is an **upper bound** of the eroded density.
- An empty sample advances `2·stepF`.

**Switch to fine.**
- At the first coarse sample with density, the march backs up `(2 − fineJitter)·stepF` and switches to
  fine steps. The switch therefore lands on a jittered lattice.
- If the back-up passes the segment start, it restarts **jittered** at the start. Clamping it to the
  start made every ray from inside cloud or rain step on a lattice aligned to the eye.

**Fine mode.**
- Fine samples read the eroded field with
  `detailAmt = 1 − smoothstep(LOD start, 4·LOD start, t)` (LOD start 13 km).
- After more than 5 consecutive empty fine samples the march returns to coarse mode.

**Cheap samples.**
- A coarse sample that is almost all high-layer ice (`thin > 0.9`) or rain shaft (`rain > 0.9`) is shaded
  right there and never enters fine mode.
- It counts for two steps' length when it is ice seen at a footprint above 60 m, or rain. These samples
  were a large share of all steps.

**Termination.** The loop ends when transmittance falls below 0.01, at the end of the segments, or at the
iteration budget (`misc.x`, 400 by default).

**Out-of-budget fill.**
- The unmarched remainder, including whole segments never reached, is filled with the path's **mean
  extinction**: `Trem = exp(−(odPath/pathLen)·remLen)`.
- It is lit with the mean colour so far, and its depth is placed at the midpoint of the remainder.
- Leaving it transparent let the horizon see through the clouds.

## 4. Lighting a sample

The radiance a sample scatters toward the eye is

```
S = keyS + ambS + bounceS + nightS
```

Phase functions use Henyey-Greenstein normalised so isotropic = 1 (no 1/4π), with `SUN_INTENSITY = 1`.

### 4.1 Phase functions (computed once per ray)

| Phase | Form |
|---|---|
| cloud single scattering | `0.6·HG(0.57) + 0.4·HG(−0.25)` (defaults) |
| multiple scattering | `0.85·HG(0.2) + 0.15·HG(−0.4)` |
| ice (`thin`) | broad lobe `HG(0.3)` + `cv2IceOptics / 0.55` |
| rain | `0.3·HG(0.97) + 0.55·HG(0.35) + 0.5·cv2RainOptics` |
| snow shafts | the rain phase mixed toward faint ice optics + pillar, by the temperature 500 m above the ground under the eye |

**Ice is delta-scaled.**
- The ice crystals' forward peak (45%) is treated as unscattered: `σ *= 1 − 0.45·thin`, and the phase
  keeps only the broad lobe.
- Full extinction dimmed the clouds under cirrus as if the peak were lost.
- In the light march the factor is harder, `1 − 0.8·thin` (g ≈ 0.8), because real cirrus barely dims what
  lies under it.

**Ice and rain optics are geometric.**
- `cv2IceOptics`:
  - 22° and 46° halos from Snell's law per colour channel (n_ice per channel);
  - sundogs through the effective index for the Sun's elevation;
  - the parhelic circle and circumzenithal arc.
- `cv2RainOptics`: primary and secondary bows, the bright interior and Alexander's band, for n_water per
  channel.
- Ice optics are gated per **region** by crystal habit (`cv2IceHabit`), evaluated once at the ray's first
  ice sample. A 22° halo appears in about a third of cirrus, the CZA rarely.

### 4.2 Key light

**Sun colour at the sample.** `sunColorAt` is cached at fixed **800 m altitude levels** and interpolated
between two levels; it is refreshed after 25 km of ray. Per level it holds:
- the Rayleigh/Mie Chapman column toward the Sun, plus an ozone shell crossed at a grazing secant
  (`sunTransmit`). A midpoint rule along a grazing path misses the tangent point's dense air.
- the 6-step zenith single-scatter sky radiance (`skyZenithAt`).

The clouds' key-light Rayleigh is its own gain ("Cloud sunlight Rayleigh"), separate from the sky's.

**Earth's shadow.** `sunVis = smoothstep(horizon ± 0.004, sun elevation at the sample)`.

**Moon as key light.** When the Sun is hidden, the Moon is the key light through the same light march, with
`moonGain × "Moon on clouds"`.

**Extra Sun terms.**
- **Eclipse.** `× eclipseSunVis(sample)`: the share of the solar disc visible from the sample.
- **Sun-path profile (`cv2SunProf`).**
  - The lightning pass marches 400 km from the eye toward the Sun and stores optical depth at 17
    quadratically spaced distances.
  - A sample near the eye's line to the Sun (a Gaussian of ~2 km about the line) takes `exp(−(depth beyond
    its point))` on its key light.
  - So the cloud between the eye and a low Sun shadows the cloud in front of it, which the few-km light
    march cannot see. Clouds beyond the blocker keep their light.
  - Behind the eye, the distance is to the eye itself, which keeps the term continuous at 90°.
- **Rain samples.** `× mix(1, sunCloudT, rain·e^(−t/100 km))`. Without it the rain phase's sharp forward
  lobe draws the Sun's disc in a shaft the Sun cannot reach.

### 4.3 The light march: one call site, four jobs

`lightAmbOD` contains the **only** lighting call of `cv2Field`. Each field call inlines ~60 KB of GPU code,
and separate call sites for the light steps, the far sample, the sky probe and the beam occlusion doubled
the shader. One loop does all four:

| k | Sample | Accumulates |
|---|---|---|
| 0 … nL−1 | geometric steps (ratio 1.6) toward the light, summing to 2500 m, each jittered over its segment. Steps 0-1 read eroded detail (self-shadowed lumps), later steps mean erosion. | optical depth `od` (ice delta-scaled by 0.8) |
| nL | one far sample at 1.6 × the light length | `od += σ·0.8·L` |
| nL+1 | probe 250 m straight up | `σUp`, for the sky ambient |
| nL+2…+4 | three steps (75 / 375 / 1275 m) along the mean beam direction | beam optical depth |

**Light LOD.** When the ray's transmittance is below 0.5, the footprint passes "Light LOD footprint" (40 m),
or the sample is rain, the steps between the first and the last collapse into one. That gives 2 steps with
the first kept; dropping the first flattened the towers' self-shadow.

**Thin samples.** Ice samples skip the light march: `od = σ·400 m`, no probe.

**Decks at grazing light.** `od = max(od, 0.5·deck·σ·(topH − h)/max(up·L, 0.03))`. A deck lit at a grazing
angle is shadowed by its path to the top, tens of km, far past the light march. Without this the
terminator band glows red from orbit.

### 4.4 Scattering the key light

```
powder = 1 − 0.29·e^(−300σ)·(0.5 − 0.5·cos θ)
keyS   = keyCol · powder · ( phSS·e^(−od) + phMS·ms(od)·msBright )
ms(od) = Σ_{k=1,2} b^k · e^(−od·a^k)          // Wrenninge-style octaves, a 0.12, b 0.43
```

`phSS` is mixed toward the ice phase by `thin` and toward the rain phase by `rain`.

### 4.5 Ambient light

**Sky from above** is the diffuse transmission of the cloud above the sample (two-stream, g ≈ 0.85), plus
light from below (ground and horizon sky):

```
τUp    = max(σUp·250 m, 0.6σ·(topH − h))
ambVis = mix(0.7, 1, hf)/(1 + 0.11·τUp) · mix(1, e^(−150σ), 0.3) + 0.35·(1 − hf)
skyAmb = skyZenith · skyDusk · ambientGain · (1 + twilight boost)
       + moonlit sky + moonless night sky light ("Night sky light")
```

- `skyDusk` fades the sky ambient over the Sun's first ~5° below the sample's horizon.
- The moonless night-sky term is ungated, so night clouds are never black.
- An eclipse multiplies `skyZenith` by `eclipseSkyLight`, the sky light inside the Moon's shadow.

**Ground bounce.** `keyCol·max(up·L, 0)·0.15·gain·(1 − hf)²`. Ice also gets light reflected from the cloud
below it, scaled by the coverage under it.

**At night** (`dayness < 0.9`):
- **City up-light** on low cloud bases: the night map at mip 3 under the sample, through the same response
  curve the sky uses.
- **Reflect beams** (`beamGather` / `beamFinish`). For each culled beam:
  - the irradiance of its disk at the sample (`beamDisk`: the Sun's disk seen in the mirror, radius =
    the footprint, edge blurred over ~the mirror width, mean irradiance = reflecting area / disk area in
    Suns), cut where the beam is blocked above.
  - Only beams landing within about a footprint of their site contribute.
  - All beams are summed first. Occlusion is marched **once** along their mean direction (the three
    `lightAmbOD` beam steps).
  - The sum is lit like the Moon: phase, multiple-scattering octaves, ice and rain phases.

## 5. Integration and outputs

**Per sample.** S is taken as constant over the step, with single-scattering albedo 1:

```
Tstep = e^(−σ·stepW);   w = T·(1 − Tstep)
L += w·S;   wAcc += w;   dAcc += w·t;   T *= Tstep
```

**Checkpoints.**
- `tHalf` is where T first drops below 0.5.
- `tq` holds the distances where T crossed 0.8, 0.5, 0.2 and 0.05. `cv2TAt` interpolates the view ray's
  transmittance at any distance from them; the beam shafts and god rays use it.

**Air between eye and cloud.**
- `attn` = the Rayleigh/Mie transmittance from the atmosphere entry to the cloud's **mean distance**
  `dAcc/wAcc`.
- The march outputs `attn·L` **only**. It adds no airlight: the air in front of the cloud is integrated by
  the sky pass and composited outside the cloud's attenuation
  ([SKY_AND_COMPOSITE.md](SKY_AND_COMPOSITE.md) §3).
- This keeps the cloud's airlight identical to the clear sky beside it. The march's own coarse airlight
  read far darker than the sky at a grazing Sun.

**Outputs.**

| Image | Format | Contents |
|---|---|---|
| colour (b10, or b17 in pass B) | RGBA16F | rgb = `attn·L` + beam shafts; a = transmittance T (× god-ray factor) |
| depth (b11, or b18 in pass B) | RG32F | x = transmittance-weighted mean distance `dAcc/wAcc` (0 = no cloud; the shaft distance on a ray with only a shaft); y = `tHalf` if T < 0.1, else −1 |

The signed-kilometre occlusion alpha the rest of the frame uses is made later, by `cloud_march.comp`
([CLOUDS_TEMPORAL.md](CLOUDS_TEMPORAL.md) §6).

## 6. Fog, dust and ice fog march

These run after the cloud loop, as their own march, because adding them to the field broke the register
budget.

**Interval.**
- The ray's stretch inside a band of `max(1.6 × fog depth, 4 × dust height, 1.5 km) + 5 km` above sea
  level, cut at the scene depth and capped at 150 km.
- The ground end is kept when the eye is above the band.
- With no surface, the interval ends at the sea-level sphere.

**Steps.** 20, with u² spacing crowding toward the dense end: near the eye when inside the band, near the
ground from above. Each step is jittered.

**Exact integration.**
- Dust and ice fog are exponential in height. Each step integrates the profile **exactly**:
  `H·|e^(−a0/H) − e^(−a1/H)|/|a1 − a0|`.
- Sampling one height per step drew the step pattern as rings on the ground from altitude.
- Samples below the ground carry no dust or ice: past the detailed terrain range the depth lands under the
  real surface.

**Lighting, once per ray at the interval's middle.**
- The Sun colour, gated by the horizon and by the map's cover overhead, times the path to the fog's top.
- The sky zenith; the Moon; city light and night-sky light at night.
- Phases: fog uses the cloud phase; dust a forward lobe with a diffuse multiple-scattering term (tan
  albedo); ice fog the halo-rich ice optics plus a sun pillar (diamond dust).

**Compositing.** Fog samples are split at the clouds' mean depth into a near and a far part and combined
with the cloud in distance order:

```
L = L_near + T_near·(L_cloud + T_cloud·L_far)
```

## 7. Beam shafts and god rays

**Beam shafts** (`beamShafts`, after the march loop, per ray, closed form):
- For each culled beam, the ray's interval inside the beam's column is intersected exactly (ray against
  cylinder).
- It is clipped to the eye, the scene depth, the beam's ground point and 60 km of altitude.
- The column is widened to at least 1.5 pixel footprints, with energy kept by (R/R₀)².
- Air and aerosol scattering along it are integrated with exact exponential columns, at the phase angle to
  the beam.
- The result is attenuated by the air to the shaft and by the view ray's cloud transmittance at that
  distance (`cv2TAt`).
- It is added to the output colour. Its weighted distance becomes the pixel's depth when there is no
  cloud, so the resolve reprojects shafts at the right parallax.

**God rays** (experimental, off by default):
- Sky pixels only, below 60 km.
- The share of the ray's single-scattered airlight that lies in cloud shadow is read from the camera-centred
  light volume (`cloud_v2_lightvol.comp`).
- It multiplies the output transmittance, which dims the sky behind.

## 8. Design constraints

- **The register cliff.**
  - On NVIDIA this shader gets either 128 registers (a small spill) or ~224 (none); the latter is 20-60%
    slower.
  - Things that flipped it: a second blue-noise read, a tile vote inside the march, a debug exemption,
    any new `CV2Field` member.
  - Check the register count (`shaders reload` in the harness) before timing a variant.
- **Code size.**
  - Two `cv2Field` call sites only: the view sample (coarse and fine share one) and `lightAmbOD`.
  - Small loops carry `[[dont_unroll]]`. The sun-colour cache's two levels are one loop, so one inlined
    copy.
- **Workgroup size.** Adaptive mode and the beam-light cull assume 16×16.

## 9. Debug views (`clouds_v2.debug_view`)

| View | Shows |
|---|---|
| 1 | iterations / budget |
| 2 | 1 − T |
| 3 | mean distance, banded every 50 km |
| 4 | weather cube at the shell entry |
| 5 | rain optical depth |
| 6 | rain at the eye (drawn by `cloud_march.comp`) |
| 7 | per-layer optical depth: red low + rain, green mid, blue anvil + towers, white high |
| 8 | beam shafts: tile light count, beams found, shaft radiance |
| 10 | god-ray shadow share |
| 11 | adaptive full-rate tiles (drawn by the resolve) |
