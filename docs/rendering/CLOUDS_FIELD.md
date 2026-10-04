# Clouds: the density field

The volumetric clouds are a procedural extinction field, `σ(p)` in 1/m. Every cloud consumer evaluates it
with the same function, so they all see the same cloud: the view march, its light march, the ground shadow,
Reflect-beam occlusion, the eye's rain map, the lightning pass and the light volume. The field lives in
`shaders/include/clouds_v2.glsl`. Its inputs are baked by `cloud_v2_noise.comp` and `cloud_v2_weather.comp`
and driven from `src/simulations/SatelliteSimCloudsV2.cpp`.

How the march samples and lights this field is in [CLOUDS_MARCH.md](CLOUDS_MARCH.md).

---

## 1. Inputs

### 1.1 The weather cube

`cv2WeatherTex` is a cube map with 1024² RGBA8 faces and 8 mips. It is baked from the 8K equirectangular
cloud map, the water map and the DEM. One dispatch per mip bakes that mip directly from the source at a
matching source LOD; there is no box downsample.

| Channel | Meaning | How it is made |
|---|---|---|
| **R** | coverage (stored as map brightness) | **Mip 0**: the map's brightness, 2×2 supersampled. **Mips ≥ 1**: a remapped brightness, chosen so that the shader's coverage formula returns the **true mean coverage** of the N×N fine taps under it. A coarse read is therefore a real cloud fraction. |
| **G** | cloud type, 0..1 across the type table | Classified from the local variance (convective texture), coverage (deep cloud), latitude (tropics / polar), ocean fraction, and the afternoon-convection term |
| **B** | precipitation | Deep cloud × convective-to-Cb type, plus some from deep stratiform |
| **A** | ground height / 8 km | The DEM, 2×2 supersampled at the mip's scale. Read at the **Earth-fixed** direction; everything else is read in the drifted frame. |

**Coverage.** Everywhere it is `cov = clamp((r·Coverage − clear)/(full − clear), 0, 1)`; the defaults are
Coverage 0.84, clear 0.12, full 0.54. The raw map's brightness used as a fraction never reaches overcast,
hence the remap.

**Read paths.**

| Function | Filtering | Used for |
|---|---|---|
| `textureLod` | hardware | early-outs; mid, high, anvil and fog lookups |
| `cv2WeatherSmooth(At)` | C1-smooth: the fractional texel coordinate is smoothstep-remapped, then one fetch | the low layer's lookup, at mip 0. The ground shadow may blend to coarser mips via `gCv2WxLod` at a low Sun. |
| `cv2WeatherBilinear` | four texel-centre fetches blended in float | Cb tower strength (mip 3). 8-bit hardware weights there stepped a wall radius every ~150 m. |
| `cv2Ground(dirE)` | alpha × 8000 at mip 1.5, Earth-fixed | cached once per field sample |

**Texel size.** Mip 0 is ~12 km per texel at a face centre and ~5-6 km near the corners (1024 texels over
90°). Comments that call mip 0 "5 km" are quoting the source map's equatorial texel.

**Evolution** (`recordWeatherEvolution`). The cube is re-baked one face per frame, with all its mips, once
a face lags by about 300 m of wind drift or the settings change. Inside the bake:
- **Advection.** The map is read through **two copies** advected back along a slow analytic curl wind
  (~4000 km waves, turning over a day). Each copy lives over its own window P (default 3 h); the two are
  offset by P/2 and weighted sin² of their phase, summing to 1. This is the flow-map double-phase trick:
  the map moves locally and never drifts from itself.
- **Growth and decay.** Coverage grows and decays under a smooth ~1500 km field with 10-30 h periods.
- **Afternoon land convection.** Coverage is raised and the type pushed toward congestus/Cb over land
  where the Sun of three hours ago stood high and the map has cloud nearby.
- `evo_wind_mps = 0` reproduces the static map.

### 1.2 Noise volumes

Three RGBA8 volumes, 128³ with 8 mips. Every cell count is a power of two, wrapped with a bitmask, so each
volume tiles exactly.

| Volume | R | G | B | A |
|---|---|---|---|---|
| **Shape** | Perlin-Worley | inverted-Worley fBm (4/8/16 cells) | inverted-Worley fBm (8/16/32) | Perlin fBm |
| **Detail** | smooth-F1 inverted-Worley fBm (no creases) 4/8/16 | same, 16/32/64 | Perlin fBm | Perlin fBm |
| **Meso** | cluster: Worley + Perlin | cumulus cells: three sizes of domed blobs, soft-unioned, Perlin-warped and clumped | closed cells (Voronoi rims of warped Worley) | Perlin fBm, mean 0.50 ± 0.057 |

**The Meso A spread.** The baked Perlin's spread is narrow: 0.50 ± 0.057. Every threshold on it is set
against that spread.

**Periods and drift.** Each volume is read through its own anchor: the observer's drifted sea-level point
divided by the period, plus wind drift × a per-anchor multiple, formed in double on the CPU and passed as
`fract`.

| Anchor | Default period | Drift (× 8 m/s wind) |
|---|---|---|
| shape | 1900 m | 1.0 |
| detail | 1800 m | 1.6 |
| cell (meso) | 32 km | 0.85 |
| cluster (meso) | 256 km | 0.6 |
| storm / storm detail | shape / detail × "Storm feature size" (12) | as their base |
| mid layer | 5.3 km | 1.0 |
| morphology | 320 km | 0.6 |

**Separate anchors per scale.** A different scale of the same volume always gets its own anchor.
`fract(a)/k` would jump when the observer crosses a period.

**LOD.** `cv2Lod(footprint, 1/period) = max(0, log2(footprint · 128 / period))` is the mip whose texel
equals the pixel footprint.

### 1.3 Morphology texture

`cloud_morph.rgba8`, built by `tools/make_cloud_morph.py` from four MODIS scenes, holds one channel per
cloud morphology:
- R: closed cells
- G: open cells
- B: cloud streets
- A: clustered cumulus

Each channel is made seamless and rank-equalised to uniform. As a result, a threshold at 1 − c covers
exactly the fraction c.

`cv2MorphZ` turns a lookup into a normal deviate. It reads triplanar through a cube projection of the
drifted sphere and weights the channels by regime:
- stratiform → closed cells;
- convective over sea poleward of ~30° → open cells;
- some convective regions → streets;
- the rest → cumulus.

**Must be bound everywhere the field is evaluated.** A consumer without it would place its clouds
somewhere else. The **detail** volume, by contrast, is bound only in the view march and the far layer.
Everyone else passes `detailAmt = 0` (mean erosion), which matches the eroded field on average.

### 1.4 Analytic fields

- **`cv2FlowDisp`**: the curl of eight smooth waves on the drifted unit sphere (period ~6000 km, slowly
  evolving). It is divergence-free, evaluated in float and capped in amplitude ("Flow warp" ≤ 0.1 of the
  period).
  - It bends the weather lookup and the cluster field, so weather systems curve.
  - It **never** bends the cells: any warp stretches every field read through it by its shear.
  - It is computed once per field sample (`gCv2FlowSet`).
- **`cv2Tropo`**: the tropopause scale by latitude, ×1.35 in the tropics down to ×0.8 at the poles. It
  multiplies every type's vertical span.
- **`cv2DirNoise`**: direction-hashed value noise for dust regions.

### 1.5 Sample positions

`cv2Pos(relENU, eyeH, enuToEcef)` builds a `CV2Pos` from an observer-relative offset. It holds:
- the altitude (cancellation-free);
- the sea-level projection;
- the unit ECEF direction `dirE`.

Noise reads use `anchor.xyz + cv2Drift(offset) · anchor.w`. Weather reads use `cv2Drift(dirE)`. Ground
reads use `dirE` itself.

---

## 2. Structure of `cv2Field`

```
cv2Field(q, detailAmt, footprint):
    gCv2FlowD   = cv2FlowDisp(drifted dir)        // once per sample
    gCv2Ground  = cv2Ground(dirE)                 // once per sample
    col = cv2ColumnSigma(...)                     // Cb towers; also sets gCv2RainCore, storm proximity
    f   = cv2FieldLow(..., stormProx = col.prox)  // low layer (stratus .. Cb regions, rain shafts)
    f  += cv2MidSigma(...)  × (1 − storm share)   // altocumulus / altostratus
    f  += col                                     // towers
    f  += cv2AnvilSigmaP(...)                     // map-fed anvil shield (off by default)
    f  += cv2HighSigma(...) × (1 − anvil presence); f.thin = high share
```

The result is a `CV2Field`. Its members are combined by `cv2Add`: extinctions sum, `topH` takes the max,
and `deck` is a density-weighted mean.

| Member | Meaning | Read by |
|---|---|---|
| `sigma` | extinction, 1/m | everyone |
| `hf` | height fraction within the cloud | powder, ambient, ground bounce |
| `msBright`, `ambient` | per-type lighting multipliers | march lighting |
| `deck`, `topH` | stratiform share, cloud top altitude | the march's path-to-top shadow term for grazing light on decks |
| `thin` | high-layer (ice) share | cheap lighting, coarse-step shading, ice phase, delta scaling |
| `rain` | set (to 1) by the low layer's below-base branch | rain phase, rain-only coarse shading, the eye's rain map |

**Register budget.** Anything added to `cv2Field` or `CV2Field`, even an unused struct member, has pushed
the march past NVIDIA's 128-register allocation, which costs 20-60%. This is why fog and dust are a
separate function.

---

## 3. The low layer: `cv2FieldLow`

### 3.1 Types

A five-row table (`cv2Types`), interpolated linearly by the weather cube's G:

| Type | base / top (m) | flatness | extinction (1/m) | erosion | billow | convective | precip |
|---|---|---|---|---|---|---|---|
| stratus | 400 / 1300 | 1 | 0.030 | 0.35 | 0.3 | 0 | 0.5 |
| stratocumulus | 900 / 2200 | 0.7 | 0.040 | 0.55 | 0.6 | 0.5 | 0.5 |
| cumulus | 1000 / 3200 | 0 | 0.050 | 0.75 | 1 | 1 | 0.5 |
| congestus | 1000 / 7000 | 0 | 0.060 | 0.8 | 1 | 1 | 1 |
| cumulonimbus | 900 / 11500 | 0.15 | 0.080 | 0.7 | 0.9 | 0.9 | 2 |

The vertical span is scaled by the tropopause factor. Nimbostratus is a stratiform deck thickened by up to
3.5 km where the map rains.

### 3.2 Early-outs

- outside the shell `[shell.x, shell.y]`;
- above the tallest low top that can occur here, when towers are on (they own the storm heights);
- coverage at mip 2 at the flowed direction below a quarter of the clear threshold.

### 3.3 Terrain-relative heights

**Bases and tops follow the ground.** The lift is
`lift = gC + (gLocal − gC) · mix(0.6, 0.9, convective)`, where gC is the lower of the local and the
~regional (mip 4) ground. The regional ground lifts in full; the relief above it lifts by 0.6 (decks) to
0.9 (convective).

**Decks.** A deck's thickness and base follow a km-scale variation `deckVar`, taken from the cells and the
closed-cell rims, so a thicker part hangs lower.

**Base bumps.** Bases are bumped in 3D by the lobes, with amplitude "Base roughness".

### 3.4 Placement: where cloud exists horizontally

One placement serves every distance. A wider footprint only filters it.

1. **Weather lookup.**
   - The flowed direction plus a small smooth warp (Perlin channels only).
   - Read C1-smooth, giving `cov`.
   - A second, coarse fraction `covF` comes from mip 2.
   - `covT = max(cov, 0.5·covF)`.
2. **Field value.**
   - `field = mix(fieldStrat, fieldConv, convective)`.
   - The convective field is built from the cumulus cells (coarse C1-smooth cells for deep types: wider
     towers), clustered by the cluster channel.
   - The stratiform field is built from the closed-cell rims and cells.
3. **Mesoscale term.**
   - The cluster field at 1×, 4× and 16× (`fz`, a normal deviate) is mixed with the imagery morphology
     (`cv2MorphZ`, "Morphology" 0.8).
   - It is added as `0.3 · Imagery share · clamp(z, ±2.5)`.
4. **Threshold.** `thr = 1 − covT`, `e = clamp((field − thr)/0.55)`.
   - `e` is "how far inside the 2D field". Everything vertical is expressed in the **same units** as `e`.
   - That is what lets noise bulge a cloud's side outward: noise added to a top height barely moves a
     steep wall.
5. **Partial cover.**
   - Cells smaller than the pixel become a sub-pixel cover fraction:
     `presFar = clamp(0.5 + (field − thr)/(2.5 σU))`, where σU is the cell variance the mip averaged away.
   - This is a haze of that opacity, never a second placement.

### 3.5 Shape: the 3D margin

Cloud exists where

```
m = m0(e, z) + A · lobes(x, y, z) · 2 − gBase(h)² / 0.3   >  0
```

- **Convective profile** (`m0` for cumulus to Cb).
  - Each cell is one cloud. Its top T comes from the cell-scale strength (cells two mips coarser, four for
    deep types), capped at "Tower top" × the span, with a minimum height so weak cells read as puffs.
  - The required strength is parabolic about the widest point (`zm` ~0.55-0.65 of T): a narrower stem and
    a round dome.
  - The result is capped by the cells' own dome (smooth min), so dense fields keep turrets instead of
    mesas.
  - "Top-heavy" sets how much narrower the stem is.
- **Stratiform profile.** `e − 1.2·smoothstep(0.3, 1, z/zTop)`: flat bases, lumpy tops, and the top height
  following `deckVar`.
- **Lobes.**
  - Inverted-Worley octaves of the shape volume, in field units.
  - Convective lobes taper with height: "Cumulus lobes (bottom)" 0.2 → "(top)" 0.07, giving smooth domes
    over lumpy bases.
  - They fade with the footprint and are damped near the base ("Base flatness").
  - Deep types read the storm-scale shape volume.
- **Base curl.** `gBase` curls the base up toward the cloud's edge.
- **Surface ramp.** `Ph = clamp(m · 4) · smoothstep(0, 60 m, height above base)`.

### 3.6 Density, erosion, edges

- **Base density.** `d = Ph · mix(0.75, 1, shape.r)`.
- **Erosion** (`detailAmt ≥ 0`):
  - The detail volume is read through a small distortion by the shape volume, mixed with storm detail for
    deep types.
  - The detail value `df` **fades with distance toward its mean (0.45), not toward zero**:
    `df = mix(0.45, fetched, detailAmt)`. Erosion therefore stays at full average strength at every
    distance; only its texture fades.
  - It inverts toward the top for billows.
  - Its strength comes from the type's erosion weight, and it is reduced deep inside dense cloud, where
    full erosion holed the interior.
  - Applied as `d = (d − erode)/(1 − erode)`.
- **Coarse steps skip erosion.** `detailAmt = −1` skips it entirely. The result is an upper bound on the
  eroded density, which the march's coarse steps rely on.
- **Final scale.** `d` is scaled by a mild height ramp and the precipitation factor, then
  `σ = d · type extinction · Density`.
- **Outputs.** `deck` = stratiform share × interior, `topH` = the column's top altitude.

### 3.7 Rain shafts

Below the base, the low layer draws rain curtains:

```
rate   = max(rainAmt · smoothstep(0.3, 0.8, e) · mix(0.4, 1, convective),   gCv2RainCore)
σ_rain = rate · shaft · 0.0012 · (1 + core)
```

- `rainAmt` is the map's precipitation.
- `shaft` is the shape volume read in a frame compressed 85% vertically and slanted downwind below the
  base. Sub-pixel curtains fade to a 0.45 mean.
- `gCv2RainCore` is a heavy-rain core under each anvil-reaching tower, 2.5× longer downwind, set by
  `cv2ColumnSigma`. That is why `cv2Field` runs the columns first.

The shell floor drops to 0 m whenever rain, fog or ice fog is on.

---

## 4. The other layers

### 4.1 Mid layer (`cv2MidSigma`): altocumulus and altostratus

- **Lens.** A lens between ~2.8 and 7 km, lifted 0.8× with the ground. Its base and thickness come from
  the cluster Perlin stretched to ±2σ, so it is not one flat sheet.
- **Regime.** Where the cluster field, biased by the weather over ~80 km ("Layer spread"), crosses a
  threshold, scaled by coverage. It is never gated on `cov > 0`: the source map's JPEG blocks would show.
- **Altocumulus** (broken low cloud): cloudlets thresholded from the mid anchor's shape volume. Past a
  400-3000 m footprint they become a haze of the regime's area fraction, taken from the closed-cell
  morphology.
- **Altostratus** (stratiform): a translucent sheet.
- **Storms.** It gives way where Cb towers can stand.

### 4.2 High layer (`cv2HighSigma`): cirrus, cirrostratus, cirrocumulus

- **Band.** At ~0.74 of the local tropopause.
- **Regime.** Two octaves of Perlin at the "Cirrus field size" (1200 km), in a frame displaced by the low
  flow × "Cirrus flow", biased toward the weather systems' coverage (mip 2-4).
- **Streaks.**
  - Coordinates: the shape volume is read with the along-wind coordinate = drifted longitude × R, at a
    period that divides the equator an integer multiple-of-4 number of times, so there is no antimeridian
    seam.
  - The along-wind axis is stretched ("Cirrus stretch") and sheared with height (fall streaks).
  - Fibres are bundled (a ¼ read) and combed (a 9× read).
- **Other forms.** Cirrostratus veils where the map is stratiform; cirrocumulus patches by a cluster
  channel.
- **Haze fallbacks.** Far away the fibres give way to a banded haze, and from inside the layer to its mean.
- **No erosion.** The layer sets `CV2Field.thin`, which makes the march light it cheaply (§ CLOUDS_MARCH).

### 4.3 Cumulonimbus towers (`cv2ColumnSigma`)

Deep convection is drawn as discrete towers, not as a margin field, because a field thresholded per height
cannot narrow and flare again.

**Lattice.**
- A 2D lattice on an equal-angle cube map of the drifted sphere.
- Cell size = max("Cb spacing", full reach / 1.12). Full reach covers the flared head, its downwind drift
  and lean.
- Centres are jittered over 0.15-0.85 of a cell, so a 2×2 search is exact to 0.65 cell and a 3×3 to 1.15.
  The 3×3 search is used once a tower's reach passes 0.62 cell, and only above mid-height.
- A tower cannot straddle a cube-face edge; towers near an edge fade out, leaving a tower-free band rather
  than a cut.

**Gating and roles.**
- Tower strength is the float-bilinear weather read at mip 3 (type ≥ ~0.55, with coverage).
- Cells are grouped in 3×3 blocks (`cv2CbRole`, shared with the lightning pass). A block holds a storm
  with probability "Cb fill". It has one **dominant** tower that reaches the anvil, plus 0-2 shorter
  **flanking** towers in a line.

**Shape per tower.**
- A weaker tower is shorter, not thinner.
- Anvil-reaching towers get a waist, a head that flares under the lid ("Cb flare"; this flared head *is*
  the anvil), downwind head drift, and on about a third of towers an overshooting dome.
- The outline bulges in turrets (3rd and 5th angular orders, turning with height). The top rounds off.
- Storm-scale lobes; a separate "Cb head lobes" strength on the head.
- A ~300 m surface ramp.
- Erosion from the storm detail volume.

**Outputs.**
- `sigma`, `topH`;
- `prox`: proximity to a tower's edge, within "Storm cumulus reach";
- `near`: under a dominant head, downwind;
- `storm`: tower strength;
- `gCv2RainCore`.

**Storm cumulus.** Around towers, the low layer's cumulus tops rise toward "Storm cumulus top" by `prox`.
Away from towers, Cb-typed map regions are capped at the cumulus top, so a region typed as storm by the map
is not a continent-wide 6 km floor.

### 4.4 Map-fed anvil shield (`cv2AnvilSigmaP`)

An optional smooth ice shield under the tropopause, fed by the weather at and upwind of the point and by
tower proximity. Off by default ("Anvils" = 0); with the defaults the anvils are the towers' heads.

---

## 5. Fog, dust and ice fog (`cv2FogDust`)

A separate function returning three extinctions, marched after the clouds
([CLOUDS_MARCH.md](CLOUDS_MARCH.md) §6). The "height above ground" here is above the **smoothed** ground
(`cv2Ground`, ~15 km), so valleys fill deeper than ridges.

| Component | Where | Vertical profile |
|---|---|---|
| **Fog** | Radiation fog: clear-to-broken nights in valleys and on low ground by the sea. Advection fog: sea and coast under a stratiform regime. Mist: in rain. Patchy on the cluster field. | flat top at "Fog depth" that undulates with the shape noise |
| **Dust** | Dry land: map clear over a region, not sea, not rain. Regional events (`cv2DirNoise`), plumes on the cluster field. Fades when seen from above. | exponential, scale "Dust height" |
| **Ice fog / diamond dust** | Antarctica, Greenland's interior, high-Arctic land, under a clear sky | exponential, 250 m |

Knockout bit 2048 turns all three off. The lightning pass also evaluates `cv2FogDust` at the eye to get the
ice-fog density used for diamond-dust glints.
