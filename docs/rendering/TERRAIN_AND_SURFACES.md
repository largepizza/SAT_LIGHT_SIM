# Terrain, scene depth and surface shading

The ground is a ray-marched height field. Three passes march it, at quarter, half and full resolution, with
the same function, so they agree on where the surface is. The half-resolution result is the frame's shared
depth, `sceneDepthImg`. The full-resolution march in `sat_sky.frag` shades the surface.

Files: `shaders/include/terrain.glsl`, `shaders/include/terrain_detail.glsl`, `shaders/scene_depth.comp`,
`shaders/terrain_probe.comp`, and the surface sections of `shaders/sat_sky.frag`.

---

## 1. The height function: H = DEM + water + detail

### 1.1 The DEM

- R8, 14999×7500 (~2.67 km per texel), land only.
- Decoded as `h = max(0, v·9000 − 15/255·9000)`. The 15/255 offset is the sea baseline; without it every
  coast is a 529 m cliff.
- **Near the observer** (within 4 km), the DEM is filtered by hand (`tdDemTexel` + `tdDemBilinear`: four
  `texelFetch` and a float lerp). The texel coordinate is built from the CPU's observer texel
  (integer + fraction) plus a local offset. This avoids both the float UV's ~2.4 m resolution and the
  hardware filter's 8-bit sub-texel weights, which together built metre-high shelves on steep walls.
- **Beyond 4 km**: one hardware-filtered fetch.
- **Every evaluation** also reads mip 3 (~21 km), the regional mean used for roughness.

### 1.2 Water

Binding "earthSpecTex" holds the **water map**, R8G8, baked by `tools/make_water_map.py` from Natural Earth
10 m:

- **R**: a smoothed signed distance to the shore (`0.5 + d/(2·19.5 km)`, d > 0 in water).
- **G**: the level of the nearest water body, in DEM units (each body's median DEM over its interior).

`waterAdjustHeight`:
- A water point returns its body's flat level.
- Land within 3 km of a shore is banked up to at least the level, and capped by a 0.25 m/m ramp from the
  shore that lets go between 1.5 and 3 km.

`tdShoreOffset` moves the shoreline by one octave of anchored 1024 m value noise (±700 m): coves and
headlands.

Water is marked by `hMip3 = kTdWaterMark (−1e4)`, which also zeroes the detail amplitude there.

The map is only fetched where its mip 2 is non-zero, i.e. within ~15 km of water.

### 1.3 Procedural detail

All detail is in **observer-anchored** coordinates: an integer 2048 m cell plus an offset, from double on
the CPU (README §4.1). Nothing swims at any longitude.

**Value octaves.**
- Eight octaves of 3D value noise on the sea-level sphere (2048 m down to 16 m), with an analytic gradient.
- **Slope damping**: `1/(1 + erode·|Σ slope|²)` gives an eroded look.
- **Amplitude**: "Detail height" (160 m) × **roughness** × a coast fade.
  - Roughness is the DEM's *signed* relief against the 21 km mean: ridges rugged, valley floors smooth.

**Erosion octaves.**
- Two octaves of 512 and 256 m cells, scaled by "Erosion size".
- **Domain**: the ECEF plane perpendicular to the dominant axis of up, blended across cube-face edges.
- **Kernel**: a 3×3 neighbourhood of jittered points with the compact kernel (1 − d²/R²)³, R = 1.25 cells.
  Each point draws a cosine stripe whose phase runs across the slope, so gullies run downhill.
- **Steering**: the DEM slope (central differences ±1 texel) plus the coarse octaves' gradient, plus the
  previous erosion octave's gradient (branching).

**Order.** The coarse value octaves (4), then erosion, then the fine value octaves (`tdOctaves`). Erosion
runs once, outside the octave loop: one call site, with `[[dont_unroll]]`. GLSL inlines everything and sizes
registers for the largest path, so duplicated erosion code slowed every terrain pixel even when switched off.

**LOD.** Geometry LOD `tdGeomLodM = max(2·pixAngle·t, 0.012·t)`. At normal FOVs the 1.2%-of-t term
dominates, so the LOD depends on distance alone. That is why the ¼, ½ and full-res passes see the **same**
surface. An octave fades in over `smoothstep(lod, 2·lod, cell)`; erosion over twice that margin.

**Shading-only extras** (not in the marched surface):
- `tdMicroBump`: gradient-noise octaves 8 m → 0.25 m, solid 3D, perturbing the normal.
- `tdMicro`: an albedo mottle.

### 1.4 The observer's ground

`observerEffHeightDetailed` is the full height function at the observer's point, max'd with the user's
height offset. The quarter pass computes it once into `terrainFrameBuf`, and every other pass reads that.
The eye is 2 m above it.

Every pass that produces or consumes a distance must use the same eye height. A pass with a different eye
gets a depth buffer that disagrees with its consumers.

---

## 2. The terrain march: `terrainMarchDetailed`

### 2.1 Bounds

- **Gates.** The ray must point below ~44° elevation and cross the `R + 9 km` shell.
- **End.** `tExit` is the sea-sphere hit, else the shell exit. It is capped by
  `mix(250 km, 3600 km, eyeAlt/400 km)` and by "Terrain distance fade end" (900 km).

### 2.2 Stepping

Per step, the DEM is read first, then three tiers:

1. **Far above everything.** The ray is above `DEM + tail bound + erosion bound`. Step by that gap; only the
   DEM was fetched.
2. **Near.** The coarse octaves are evaluated, and the gap is taken against `coarse height + remaining
   octaves' bound` (`tdRemainingBound`). Only if that gap is ≤ 0 is the full height (with erosion)
   evaluated.
3. **Hit.** `0 ≤ gap < pixel footprint`, i.e. within one pixel of the surface.

**Step size.** `clamp(0.6·gap, minStep, maxStep)`, with
`minStep = max(0.5 m, t·mix(1.5%, 5%, (i/maxSteps)²))` and `maxStep = clamp(0.3t, 20 m, 12 km)`.

**Refinement.** A negative gap brackets the crossing, which is refined by 3 regula-falsi steps on
`terrainHeightLinEro`. The erosion is linearised as the plane left by the last full evaluation, so a bracket
costs no extra erosion calls.

**Landing on the exit.** A step that would pass `tExit` lands **on it** once (`atExit`), so a crossing in
the last step is still bracketed. Without this, steep rays over low land jumped past the sea sphere and drew
flat sea-level land in rings about the nadir.

**Out of budget is not a miss.** A tail march (≤ 48 steps, coarse octaves, no erosion) and a bisection
finish the ray. Returning "sky" made the depth say sky and punched holes through distant hills.

### 2.3 The seed pyramid

The march is the most expensive per-pixel work at ground level. Each pass starts each ray from a coarser
pass's answer:

| Pass | Resolution | Starts from | Writes |
|---|---|---|---|
| `scene_depth.comp`, quarter | ¼ | the eye | `sceneDepthQImg`; `terrainFrameBuf` (eye ground) |
| `scene_depth.comp`, half | ½ | min of its 2×2 quarter texels × 0.995 − 0.5 m; **skips** rays whose four seeds were all sky | `sceneDepthImg` = min(terrain / sea, mesh distance) |
| `sat_sky.frag` | full (or render scale) | min of its 2×2 half texels, same rule | its own hit, for shading |

**Why seeding is conservative.** A coarser pass's hit tolerance (one of *its* pixels) is wider, so it stops
earlier and counts near misses of a crest as hits. A seed is therefore never past the surface the finer
pass will find. Each pass's descriptor set samples the *other* image, so neither is read in the layout it is
being written in.

### 2.4 Terrain sun shadow

`terrainSunShadow`: 16 steps toward the Sun on the coarse surface (3 octaves).
- **Start**: biased along the normal and lifted onto the coarse surface. Starting on the detailed surface
  gave texel-sized acne.
- **Penumbra**: from `min(6·gap/t)`.
- **Stop**: above 9.6 km or past 50 km.

---

## 3. The shared scene depth

`sceneDepthImg` is half the swap extent, R32F, holding linear metres along the view ray to the first
terrain, sea or **mesh** surface; `1e30` means sky. Every later pass tests against it instead of deriving
its own occlusion:

- **Clouds.** Each march ends at it, with the soft contact fade.
- **Beams.** Pointing rays stop at it.
- **Sky pass.** The joint-bilateral cloud upsample weights by it, and its own march is seeded from it.
- **Flare sources.** Bloom seeds of satellites are hidden behind it.

Knockout bit 1024 skips both scene-depth passes.

The hardware depth buffer uses a different, log encoding; see README §4.2.

---

## 4. Surface classification in `sat_sky.frag`

After the full-res march:

1. **Hit on water.** If the hit is on water (`hMip3 ≤ kTdWaterMark`):
   - a **lake** takes the ocean path at its own level (`tSeaLvl = tHit`, `waterLevelM`);
   - the **sea** voids the hit.
2. **Miss that meets the sea sphere.** The water map at that point decides:
   - **land** at 0 m (deltas read 0 in the DEM) becomes a terrain hit at the sphere;
   - **water** stays sea.
3. **Land past the march range** (not in env probes). Two fixed-point re-intersections place the ground on
   the sphere `R + DEM height`, so far plateaus are not drawn at sea level under the whole air column.
4. **Mesh.** A mesh nearer than all of this is the pixel's surface.

`tSurface` is where the atmosphere loop stops (SKY_AND_COMPOSITE §1).

---

## 5. Land shading

### 5.1 Normal

The normal combines:
- the DEM gradient;
- the detail gradient at the shading LOD (3 px, with the hit's erosion plane);
- micro bump (reduced on day-map snow).

### 5.2 Albedo

In order:

1. **Day map** (8K, `textureGrad` with the antimeridian derivative fixed).
2. **City day detail.**
   - The legacy tiled texture where streets stop resolving.
   - The procedural city layout (§7) as a **ratio to its own expected mean**, so it converges to exactly
     the map once unresolved.
3. **Farms, solar parks, beaches** (§7), with the same ratio-to-mean rule.
4. **Procedural material.**
   - Rock on steep faces, snow shed from steep faces, an AO from the detail height, the `tdMicro` mottle.
   - It fades out by a 60-400 m footprint, so orbit views show the day map untouched.
5. **Close-up material textures.**
   - Six CC0 sets (grass, forest floor, rock, snow, sand, dirt) in a `sampler2DArray` (binding 27, the
     stage's last sampled image).
   - **Selection**: picked from the day map (green → grass, dark green → forest floor, bright unvegetated →
     sand, else dirt; steep → rock; map white → snow; beach → sand). The two strongest are sampled.
   - **Sampling**: biplanar in ECEF on the anchored 4 m lattice with an explicit LOD, blended by height,
     anti-tiled by a 16 m second read.
   - **Application**: the albedo is a ratio, so the hue at distance is unchanged; the texture normal
     perturbs the shading normal; AO joins.
   - Fades in below a 0.6-3 m pixel footprint.

### 5.3 Lighting

| Term | Form |
|---|---|
| **Gates** | `dayFrac` = geographic horizon of the Sun at the point (independent of the shading normal); `twilightFrac`; `sunDiscVis` = the Sun above the point's dip horizon × the eclipse factor |
| **Direct sun** | `albedo · sunTint · (sunLit · mix(0.15, 1, terrainShadow) + bounce) · cloudShadow · dayFrac · sunDiscVis`. `sunLit` reaches a 0.05 floor smoothly; `bounce` ∝ the day map's luminance × the Sun's local height. |
| **Sun colour** | the transmittance toward the Sun (`optDepth`), hue-normalised (`sunSpecTint`), with its brightness kept separately (`sunTransMax`) |
| **Cloud shadow** | `cloudMarchTargetB.a`, a [1 2 1]⊗[1 2 1] tent at 1 half-res texel, which averages the shadow's 2×2 ordered jitter exactly |
| **Sky light** | `albedo · skyAmbientTerrain · 0.4 · "Terrain sky light" · skyView · twilightFrac`. `skyAmbientTerrain` is a 4-step zenith single-scatter integral from the hit, with the eclipse factor; `skyView = 0.5 + 0.5·n·up`. |
| **Moon** | direct `max(n·moon, 0)` × the phase-law brightness × moon gain |
| **Night sky** | `albedo · (moonlit sky + "Night sky light") · skyView`: dark, but the relief reads |
| **City lights** | the night map **minus its blue base** (`cityLights = max(night − (0.006, 0.006, 0.0132), 0)`), limited to a terrain contour above the regional ground (`cityTerrainLimit`), × the procedural pattern (§7), + street-lamp pools lighting the final albedo. Under cloud the finished light is blurred by the cloud's opacity. |
| **Extras** | wet paddies (Fresnel sky + glint), solar-park glass glint |

```
surface = (direct + skyLight + nightSky) · AO + cityLight + moon
```

Reflect-beam ground spots are added after the land or sea branch: a soft dome of the beam's footprint,
which follows the surface's brightness and also glints off solar panels.

---

## 6. The sea

### 6.1 Wave field

- The field (`seaMap` / `seaMapDetail`) is **exactly periodic** over 28571 × 21429 m:
  - the noise lattice is hashed mod 1200 cells;
  - octaves use an integer matrix [2 1; −1 2] with integer scales;
  - a periodic 2D warp is applied.
- The CPU wraps the observer's world offset into one period in double (`cloud.oceanState.xy`). The pattern
  is seamless and small at any distance flown.
- Height above the water is formed as `eyeH + 2 − level + t·dir.z`, avoiding cancellation.

### 6.2 Sea state

The wave amplitude multiplier comes from three sources:
- the weather cube (storm = cover × convective type + rain);
- the westerly belts;
- a regional wind.

Choppiness follows the amplitude.

### 6.3 Shore

Using the same waterline as the height function (`shoreSignedDist`):
- waves shoal to 0.4 over 150 m;
- breaker lines roll in;
- swash runs at the edge;
- the waterline is anti-aliased over a pixel footprint, toward wet sand on the land side.

### 6.4 Normal

- **Wave trace.** Within 5 km: 5 coarse steps to the **first** crossing, then 6 secant steps. The secant
  alone converged on the back faces of crests.
- **Detail normal.** Central differences on the detail field, out to "Sea wave range" (100 km).
- **Footprint filtering along the view.** An octave fades as its cell approaches 2 along-view footprints.
  Unfiltered, a rough sea under a low Sun aliases into crawling glitter.
- **Lost slope becomes roughness.** The slope the filter removes goes into `seaRough`, computed everywhere,
  so there is no line at the wave range.
- **Wind roughness.** A base roughness from the sea state that grows toward grazing views (Cox-Munk ripples
  the field does not model).
- **Far-sea ripple.** Where every octave is filtered out, a ripple normal on cells of ~5 along-view
  footprints, two levels with renormalised contrast.

### 6.5 Shading

| Term | Form |
|---|---|
| **Fresnel** | `min((1 − n·v)³, 0.5)` |
| **Reflection** | the sky along the reflected ray, tilted up by roughness. It is its own atmosphere march with u² spacing (a uniform 6-sample march over ~1000 km of grazing path turned the horizon tan). Below-horizon facets take 0.2 × the horizon's light. |
| **Reflected clouds** | taken from the **on-screen** half-res cloud targets at the reflected direction's projection: 5 vertical taps spread by distance and roughness, faded over 30% past the frame edge, opacity-weighted per texel, leaning back to the clear march for far clouds. Under an overcast the clear-march share is darkened by the sea point's cloud shadow. |
| **Body** | water colour × the irradiance on the sea (sun + sky, with the cloud shadow) |
| **Sun glint** | a **Beckmann** microfacet lobe in the surface's π·L/E units. Slope variance = Cox-Munk(7 m/s × sea state) + 0.06 × the footprint's lost slope + the Sun's disc. Schlick F (F0 0.02), Smith G, × the Sun's true transmittance. Seen from orbit it forms the glitter path. |
| **Foam, whitecaps, surf, wet sand** | **diffusers** lit by the sun, sky irradiance and Moon (never by the mirrored sky). Crest foam near, the mean whitecap fraction (~Monahan) far. |
| **Satellite glints** | `sat_flare.comp`'s list of satellites bright enough to glint on the water (≤ 512). Each is tested against the reflected direction, screen-space cloud and terrain at its projection, and gated by gain and floor. Skipped entirely by day or above ~8 km. |

---

## 7. Cities, farms, solar parks, beaches

All of these share two techniques:

1. **A world-fixed 2D frame** (`cityFrame`): the erosion's cube-face projection on the anchored lattice. Any
   lattice whose cell divides 4096 m is world-fixed. Rotated lattices are measured from a fixed **area**
   origin, never a moving one.
2. **Ratio to the expected mean.** A pattern multiplies the 8K map by `pattern / E[pattern]`, so as it stops
   resolving (by pixel footprint) it converges to exactly the map. Orbit views stay identical to the map,
   and the hand-off is seamless.

### 7.1 City layout (`cityLayout`, built once per pixel)

- Jittered-Voronoi districts of 4096 m.
- One street grid per 4×4-district region, so streets run straight through district borders. Some districts
  lay out their own grid, giving T-junctions.
- Nine regional styles by soft continental boxes set the geometry per region: spacing, long blocks,
  north-aligned grids, organic warps, perimeter and slab blocks. Colours (roofs, lamp types) blend per
  pixel.
- Street existence and density follow the night map at the pixel.

**Day side** (`cityDayAlbedo`, `cityDayFar`):
- asphalt streets and arterials as exact box-filtered coverage;
- lots, roofs and commercial strips;
- individual tree crowns;
- parks in the voids;
- km-scale structure for 80 m-4 km footprints.

**Night side** (`cityLightPattern`, `cityLightFar`):
- **Lamp posts** close up, each a pool on the road plus a head, each with its own lamp type and colour.
- **Glitter** (`cityGlitter`) at every distance beyond:
  - a world-fixed lattice of cells tracking ~4 pixel footprints, two levels cross-faded;
  - a point in a cell with probability set by the night map's density, lognormal weight;
  - an **anisotropic** point spread: stretched along the view's ground direction by the stretched footprint,
    so a far town is dots on screen and not horizontal smears;
  - slow scintillation.
- **Real major roads** (Natural Earth, a cell-listed storage buffer), dotted the same way.

LOD footprints ("Lamp posts to", "Street grid to", "Major roads to", "Street layout to") set where each
layer ends.

### 7.2 City sprites

- `city_sprites.comp` appends far city lights to the **satellite point list** (POINTS_AND_MESHES §6), as
  crisp points.
- They appear only where a pixel spans more than "City sprites from" (25 m) of ground.
- Where they carry the light, the ground glitter keeps only "Ground glitter under sprites" of it.

### 7.3 Farms (`farmDayAlbedo`)

- Where the day map is cultivated: not forest, desert, snow, steep, city, or high ground; settled land.
- Fields are recursive rectangle splits in three regional styles: GRID with pivots, STRIPS, PADDIES (wet
  paddies reflect).
- Fades between 0.55 and 0.9 of "Ground pattern range".

### 7.4 Solar parks (`solarSiteAt`)

- Up to 8 nearby parks, from the CPU in double, relative to the observer's sea-level point.
- Plots, blocks and rows drawn as exactly box-filtered pulse trains.
- Coverage **as seen**: a tilted panel's projection along the view, and its shadow along the Sun.
- Trackers turn toward the Sun or toward a landing Reflect beam.
- Glass is a Beckmann lobe, so each beam's light glints off other rows.
- Where a park overlaps a city, its share becomes rooftop panels.

### 7.5 Beaches (`beachAt`)

Sand 40-140 m wide on low, gentle shores, wet near the water, with the close-up sand texture.

---

## 8. Aerial perspective on surfaces

The surface is not lit through a separate fog. The atmosphere loop in `sat_sky.frag` integrates from the
atmosphere entry up to `tSurface`, accumulating the camera-side optical depth. Then:

```
color += surface · exp(−(βR·odR_cam + 1.1·βM·odM_cam))
```

The in-scattered light along the same stretch is already in `color`. Meshes take the same attenuation.
Details: SKY_AND_COMPOSITE §1.
