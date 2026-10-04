# The sea

How the ocean and lakes are drawn: the wave field, the sea state that drives it, the shore, the filtering that
keeps it from aliasing, and the reflection, glint and foam terms that light it. Where water is (the water map,
lake levels, the sea-versus-terrain decision) is on [Terrain](terrain.md); the sky the sea reflects is on
[Atmosphere and sky](atmosphere-and-sky.md).

## Where the ocean path runs

`sat_sky.frag` takes the ocean branch for a pixel when the terrain march found no land and the surface is
water: by the march's own shoreline where it ran (`waterPx`), else by the water map (`r > 0.5`). See
[Classifying the hit](terrain.md#classifying-the-hit).

A **lake** takes the same path at its own surface level `waterLevelM` (0 for the sea). Every height above the
water is formed as

\[ h = h_{\text{eye}} + 2 - \text{waterLevelM} + t \cdot d_z \]

rather than `length(p) - R_EARTH`: at 6.4e6 m a float position resolves only about 0.5 m, coarser than the
waves.

## The wave field

The field is adapted from "Seascape" by Alexander Alekseev (TDM, 2014). `seaHeight()` sums octaves of a
ridged value-noise pattern (`seaOctave()`: a noise-warped triangle-wave ridge raised to a choppiness power):

| Constant | Value | Meaning |
|---|---|---|
| `kSeaFreq` | 0.056 per m | octave-0 cell 17.9 m north-south; x is stretched by 0.75 (23.8 m east-west) |
| `kSeaHeight` | 2 | octave-0 amplitude, x the sea state |
| `kSeaChoppy` | 3 | crest sharpness, plus the sea state's term |
| ridge period | 6 cells | about 107 m between ridges |
| `kSeaSpeed` | 0.75 | the ridges travel at about 13 m/s, a real 107 m wave's speed |
| octave step | x 2.24, turned 26.6 deg | amplitude x 0.22 per octave; time x 1.9 |

Each octave adds two ridge patterns, one on the octave's lattice and one on the 45-degree lattice
\( [1\,1;\,-1\,1] \), so ridges do not line up into a square grid. Before the octaves, the whole field is
warped by two periodic 2D noises (24 and 8 cells, about 430 m and 140 m; "Sea warp" and "Sea warp detail").
The warp's local shear stays below 1, so it cannot fold the field.

`seaMap()` (the trace) evaluates "Sea octaves" octaves; `seaMapDetail()` (the normal) "Detail octaves".

### Exactly periodic

The field repeats exactly over \( 28571.4 \times 21428.6 \) m (`kSeaPeriodX`, `kSeaPeriodY`), i.e.
`kSeaCells` = 1200 octave-0 cells:

- the noise lattice is hashed modulo 1200 cells;
- ridges come every 6 cells, which divides 1200;
- the octave matrix \( [2\,1;\,-1\,2] \) and the per-octave scales `kSeaOctA` = {1, 2, 3, 5, 8, 14, 24, 41} are
  integers;
- every octave's argument is reduced modulo 1200 cells, and the warp lattices (periods 50 and 150) divide it.

Because of that, the observer's position can be wrapped into one period without any seam. The CPU accumulates
the observer's east/north travel in double (`cityOffsetEastM/NorthM`, ignoring jumps over 200 km) and sends it
modulo the period as `cloud.oceanState.xy`; the shader reads waves at `hitPt.xy + oceanState.xy`. The
arguments stay small however far the observer flies, so the waves never lose float precision and never slide
against the land.

!!! warning "Invariant: keep the field periodic"
    Any new octave, warp or ripple must have an integer period that divides `kSeaCells`, and its argument must
    be reduced modulo that period. A non-integer factor breaks the wrap: the pattern jumps whenever the
    wrapped offset crosses a period.

## Sea state

`oceanSeaState()` returns the wave amplitude as a multiple of the base sea, from the surface direction:

- **Weather**: the clouds' weather cube at mip 3 (about 40 km), turned by the cloud drift.
  `storm = ss(0.45, 0.85, cover) x (0.35 + 0.65 ss(0.35, 0.8, convective type)) + 0.6 x rain`.
  See [Weather](clouds/weather.md).
- **Westerly belts**: a band of \( |\sin \text{lat}| \) from 0.5-0.71 to 0.87-0.94 (roughly 30-45 deg to
  60-70 deg).
- **A slow regional wind**: two long sinusoids over the globe.

\[ s = \frac{0.3 + 0.35\,\text{wind} + 0.35\,\text{belt} + 0.9\,\text{storm}}{0.65}, \qquad
   \text{seaState} = \max(0.25,\; 1 + (s - 1) \cdot g) \]

with \( g \) = "Sea state from weather" (0 gives a constant sea). The amplitude scales by `seaState`, and the
choppiness rises with it (`clamp((seaState - 1) x 1.2, -1.2, 2)`).

## The shore

With the detail on and the water within 20 km, `shoreSignedDist()` gives the distance to the waterline in
metres, from the same water map and coves (`tdShoreOffset()`) the land uses, so sea and land meet on one line.

- **Shoaling**: wave amplitude falls to 0.4 x over the last 150 m.
- **Breakers**: lines rolling in, `wave^8` of a sinusoid in the shore distance travelling shoreward, between
  3 m and 10-90 m from the line, scaled with the sea state.
- **Swash**: full surf within 4 m of the line.
- **Surf** fades out as the pixel footprint passes 4-30 m.
- **The waterline is anti-aliased half from each side**: on the water side the colour blends toward wet sand
  over one pixel footprint; on the land side the beach blends toward a dark water albedo
  ([Beaches](cities.md#beaches)).

## Finding the wave surface

Within 5 km and below about 8 km of eye altitude, the ray is traced against the waves around the sea-sphere
hit, over \( \pm \min(60\,\text{m},\; 2.5 \max(1, \text{seaState}) / \cos\theta) \) along the ray:

1. five coarse steps to the **first** crossing (a secant alone converges on the back faces of crests);
2. up to six secant steps, stopping at 1 mm.

Beyond, the sea-sphere hit is used and the waves are carried by the normal.

## The normal, and filtering by footprint

The normal is a finite difference of `seaMapDetail()`. Without filtering, a rough sea under a low Sun aliases
into crawling glitter: the sharp glint and below-horizon reflection amplify any normal noise.

- **Footprint along the view.** \( f = \text{pixAngle} \cdot t / \max(|d \cdot \text{up}|, 0.02) \times \)
  "Sea wave sharpness". Each octave `i` is weighted by
  `clamp(cell_i / f - 1, 0, 1)`, so it fades as its cell approaches two along-view footprints, and the loop
  stops at the first octave fully filtered out.
- **Lost slope becomes roughness.** The slope the filter removes is not dropped:
  `seaRough = clamp(log2(f / 2) / 3, 0, 1) x clamp(seaState, 0.25, 2) / 2`, computed everywhere so there is no
  line where the waves end.
- **Wind roughness.** A base roughness from the sea state that grows toward grazing views,
  \( (0.3 + 0.2\,\text{seaState}) \cdot (1 - \text{ss}(0.03, 0.4, |d \cdot \text{up}|)) \): the wind ripples
  (Cox and Munk) the field does not model. Without it a calm sea mirrors clouds and stars crisply to the
  horizon.
- **Wave range.** The waves fade out between `(1 - fade) x range` and "Sea wave range" (100 km, fade share 0.4).
- **Altitude.** The waves fade with eye altitude over 20-60 km (the footprint filter does the rest); the trace,
  mirrored features and satellite glints fade over 3-8 km.

### Far-sea ripple

Where every wave octave is filtered out, the far band would be a mirror. A ripple normal fills it:
two levels of periodic noise (`seaRippleLevel()`), with cells of about "Far sea ripple size" (5) along-view
footprints, so they are always resolvable, plus a component on the 45-degree lattice. Each level is one
function of its index, and the blend between adjacent levels is renormalised for contrast, so no rings form
around the observer. The ripple tilts the normal mostly along the view: long, thin bands on screen, like the
far sea.

## Shading

| Term | Form |
|---|---|
| Fresnel | \( \min((1 - n \cdot v)^3, 0.5) \) for the reflection weight |
| Reflected direction | `reflect(dir, n)` tilted up by `0.12 x seaRough`, darkened by `1 - 0.45 seaRough` (half of the roughened lobe sees the next wave's water) |
| Below-horizon facets | a steep facet mirrors the next wave, not the sky: the reflection falls to 0.2 x the horizon's light over a few degrees below it |
| Sky reflection | its own atmosphere march along the reflected ray, "Refl samples" (6) samples on **u-squared spacing**. Uniform samples over a 1000 km grazing path overestimate extinction and turn the reflected horizon tan. |
| Body | `kSeaBase x seaE` plus a forward-scatter term, where `seaE` is the Sun and sky irradiance on the sea (with the cloud shadow), like the foam. Mixed with the reflection by the Fresnel. |
| Crest | a little water colour above the mean height, fading within about 300 m |
| Moon glint | a Phong lobe (power 120) x the Moon's brightness, by night |

### Reflected clouds

The clouds in the reflection are read from the **on-screen** half-resolution cloud targets at the reflected
direction's projection (not in environment probes):

- five taps along the screen's vertical, spread by distance and roughness;
- weighted per texel by opacity, since the filtered alpha mixes in the no-cloud marker;
- faded over 30% past the frame edge, so the fade lies outside the frame;
- leaning back toward the clear-sky march with roughness and past 3-30 km;
- **under an overcast**, the clear-sky share is darkened by the sea point's own cloud shadow
  (`mix(0.35, 1, cloudShadow)`), so the sea is not brighter than the cloud deck over it.

The Milky Way and the aurora are reflected too ([Aurora and airglow](aurora-airglow.md)): the Milky Way at
mip \( 3 + 4\,\text{seaRough} \), scaled by "Ocean MW refl" (Atmosphere tab); the aurora by a 6-step march along
the reflected ray. Both are hidden behind reflected clouds and fade with the wave range.

### The Sun glint

A Beckmann microfacet lobe, in the surface units (\( \pi L / E \)):

\[ \sigma^2 = 0.003 + 0.00512\,W + 0.06\,\text{seaRoughFoot} + 2 \times 10^{-5}, \qquad W = 7\,\text{m/s} \times \text{seaState} \]

the Cox and Munk slope variance, plus the slope the footprint filter removed, plus the Sun's disc. Schlick
Fresnel (F0 = 0.02), Smith masking (Walter's rational fit) for view and light, times the Sun's real
transmittance (`sunTransMax`, since the tint is hue-normalised), capped at 400. Because the filtered slope
widens the lobe instead of flattening it, the glitter path survives at every distance, from orbit included.

### Foam, whitecaps, surf and wet sand

All four are **diffusers**: lit by the Sun on the facet, the terrain's sky irradiance and the Moon, never by
the mirrored sky (whitewater showing a reflected image reads as liquid metal).

- **Whitecaps** grow over sea state 1.05-2.3. Near, crest foam where the crest value passes 0.6-0.76 (the top
  few percent), broken by noise; far, the mean foam fraction (up to 12% x "Whitecaps", after Monahan) as albedo.
- **Surf and wet sand**: see [The shore](#the-shore).

## Satellite glints on the water

`sat_flare.comp` lists up to 512 satellites bright enough to glint (`effectFlare` of at least 1) in
`OceanGlintBuf`. For each sea pixel, `sat_sky.frag` adds a sharp lobe (power 80) where the reflected view
direction meets a listed satellite, scaled by its brightness. Per entry: skipped below "Flare refl floor"
(about 37.9), below the limb, more than about 26 deg off the reflected ray, or behind the camera; then hidden
by on-screen cloud and terrain at its projection.

The whole loop is skipped by day and above about 8 km (`(1 - dayFrac) x altFade x gain` is zero): 512 entries
with two texture fetches each are several milliseconds of the sky pass from orbit.

## Settings

Stored in `settings.json` under `clouds`. Ocean tab unless noted.

| UI label | Key | Default | Effect |
|---|---|---|---|
| Sea octaves | `ocean_sea_octaves` | 3 | octaves in the wave trace |
| Detail octaves | `ocean_detail_octaves` | 5 | octaves in the normal |
| Refl samples | `ocean_refl_samples` | 6 | sky-reflection march samples |
| Sea state from weather | `ocean_sea_state` | 1 | 0 = a constant sea |
| Whitecaps | `ocean_whitecaps` | 1 | foam amount |
| Sea wave range (km) | `ocean_wave_range_km` | 100 | where the wave normal ends |
| Sea wave range fade | `ocean_wave_range_fade` | 0.4 | share of the range it fades over |
| Sea wave sharpness | `ocean_wave_sharpness` | 0.5 | x the footprint the octaves are filtered at; higher = softer |
| Far sea ripple | `ocean_far_ripple` | 1.5 | ripple strength in the far band |
| Far sea ripple size (px) | `ocean_far_ripple_size` | 5 | ripple cell in along-view footprints; lower = finer, more shimmer |
| Sea warp | `ocean_warp` | 3.5 | the 430 m warp |
| Sea warp detail | `ocean_warp_detail` | 1.2 | the 140 m warp |
| Ocean flare refl | `ocean_glint_gain` | 0.02 | satellite glints on the water |
| Flare refl floor | `ocean_glint_min_flux` | 37.9 | faintest satellite (effectFlare) that glints |
| Ocean MW refl (Atmosphere tab) | `ocean_mw_refl_gain` | 0.4 | Milky Way in the sea |

The graphics presets set the three sample counts (Planetarium and Potato: 3 / 5 / 3 and no ocean reflection,
knockout bit 8; the others 3 / 5 / 6).

## Debugging

Harness `debugview`:

| Name | Id | Shows |
|---|---|---|
| `oceanrefl` | 40 | the reflection colour |
| `oceanfresnel` | 41 | Fresnel, reflection darkening, reflection weight |
| `oceanstate` | 42 | sea state, crest value, wave blend (1 = waves filtered out) |
| `oceansurf` | 43 | the surface before glints |
| `oceannormal` | 44 | the wave normal |
| `oceanshore` | 45 | the signed shore distance |
| `oceanshadow` | 50 | the cloud shadow on the sea and the reflected-cloud weight |

## Where in the code

| File | What |
|---|---|
| `shaders/sat_sky.frag` | `seaHeight()`, `seaMap()`, `seaMapDetail()`, `seaRippleLevel()`, `oceanSeaState()`, `shoreSignedDist()`, the ocean branch of `main()` |
| `shaders/sat_flare.comp` | the satellite ocean-glint list |
| `src/simulations/SatelliteSim.cpp` | the world offset and its wrap into `oceanState` |
