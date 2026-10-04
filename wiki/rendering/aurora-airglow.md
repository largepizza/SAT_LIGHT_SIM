# Aurora and airglow

The emissive upper atmosphere: the faint airglow that every night sky carries, and the aurora around the
geomagnetic poles. Both are drawn as emission added to the sky; neither lights the ground. The scattering
atmosphere they sit in is on [Atmosphere and sky](atmosphere-and-sky.md); the half-resolution compute pass that
draws the aurora is shared with the clouds ([The cloud march](clouds/march.md)).

## Where each part is computed

| Part | Pass | Resolution |
|---|---|---|
| Green and sodium airglow | `sat_sky.frag`, inside the atmosphere loop | full |
| Red airglow | `cloud_march.comp`, `airglowRedMarchCS()` | half |
| Aurora volume and sheets | `cloud_march.comp`, `auroraMarchCS()` | half |
| Aurora in environment probes and mirrors | `sat_sky.frag`, `envAurora()` (`SKY_ENV` only) | probe |
| Aurora and airglow reflected in the sea | `sat_sky.frag`, ocean branch | full |

The half-resolution results are added to the cloud composite's in-scattered light (`B_total`, stored in the
cloud target A), which `sat_sky.frag` composites as `color x cloudB + cloudA`. The aurora therefore respects the
shared scene depth: terrain in front hides it ([Terrain](terrain.md#the-seed-pyramid)).

## Airglow

Three emission bands, each a Gaussian shell in altitude:

| Band | Line | Peak | Half-width | Colour (linear) | Gain |
|---|---|---|---|---|---|
| Green | O I 557.7 nm | 96 km | 9 km | (0.35, 1.0, 0.25) | "Airglow green" |
| Sodium | Na D 589.3 nm | 90 km | 6.5 km | (1.0, 0.65, 0.15) | "Airglow sodium" |
| Red | O I 630 nm | 275 km | 75 km | (1.0, 0.12, 0.05) | "Airglow red" |

The density at a sample is \( \exp(-((h - h_{\text{peak}})/w)^2) \) times the step length, all times
"Airglow gain" and `kAirglowScale` (5e-7).

**Day and night per sample.** Each sample is gated by its own geographic day/night, not the observer's:
`night = 1 - clamp((up . sun + 0.15) / 0.3, 0, 1)`. Looking across the terminator, the night side glows and the
day side does not. There is no Earth-shadow test beyond that.

**Patchiness.** Each band is multiplied by `0.6 + 0.4` of a slowly drifting warped Perlin noise over the sky
direction, and by a coverage mask (`smoothstep(-0.2, 0.5, noise)`, a separate seed per band) mixed in by
"Airglow coverage". Real airglow is structured into patches and gravity-wave bands, not a uniform shell.

**Polar boost** (red only): \( 1 + \text{"Airglow polar boost"} \times \text{ss}(45^\circ, 12^\circ, \theta) \)
times a coverage mask, where \( \theta \) is the geomagnetic colatitude: the red line brightens toward the
auroral zones.

**Where they are marched.**

- Green and sodium peak inside the main atmosphere loop's range (its ceiling is about 100 km), so they accumulate
  in that loop at no extra march cost.
- Red peaks at 275 km, well past that ceiling. Extending the main loop for one band would coarsen the
  near-surface scattering everything else depends on, so red has its own 16-step midpoint march from 100 km to
  500 km, at half resolution in `cloud_march.comp`. The march is cut at the scene depth. Its legs follow the eye:
  from below the shell, inside it, or above it (the ray direction and the eye's position select the near or far
  root, so the march works with the observer flying through the band).

**Variants.** `SKY_LITE` compiles out the green and sodium terms; Planetarium and Potato knock out the red march
(bit 16); environment probes keep green and sodium but have no red airglow.

## Aurora

### The geomagnetic frame

The oval is centred on the geomagnetic pole, `kGeomagPoleECEF` = (0.0481, -0.1543, 0.9868), a dipole whose
antipode serves the southern hemisphere: each sample picks the pole on its own side. `auroraFrame()` gives a
sample's geomagnetic **colatitude** and **azimuth** around the pole.

### The oval

`auroraOvalMask()`:

- **Centreline** at 20 deg colatitude, rippled by a slowly drifting noise in azimuth (+-4 deg), and moved
  8 deg equatorward at full "Storm strength".
- **Width**: a half-width of 6 deg x (1 + 1.5 storm), full within half of it and gone at twice it.
- **Coverage** (`auroraCoverage()`): a warped Perlin over azimuth and colatitude, thresholded from 0.2 (quiet)
  to -0.6 (full storm), so a storm fills the oval with arcs and a quiet night shows patches. Its frequencies and
  drift are the "Coverage" sliders.

### The curtain volume

The volume lives between 95 km (`kAuroraShellInnerM`) and 300 km (`kAuroraShellOuterM`), with soft sigmoid
edges (7.5 km below, 15 km above). Per sample (`auroraCurtainSample()`):

- **Night gate** at the sample, as for airglow.
- **Column window**: each azimuth column has its own height range inside the shell, read from the noise bake,
  with 12 km soft edges, so curtains have varied bottoms and tops.
- **Folds** (`auroraCurtainNoise()`): curtain noise stretched to be fine across the azimuth (frequency 40 per
  radian, about 55 km cells, finer in a storm), coarser in colatitude (about 94 km), and long in altitude
  (about 167 km): many separate folds, each a long vertical streak. A live warped noise ("Fold shimmer rate")
  shifts the folds' phase over time.
- **Colour** by altitude: green at the base, toward red-magenta (0.65, 0.15, 0.45) at the top; it blends into the
  airglow colours at both shell edges, so the aurora meets the airglow layers without a seam.

### The noise bake

Sampling warped noise per step is what made the aurora costly, so the fold and column noise is baked once at
init by `shaders/aurora_noise.comp`: a 1024 x 16 x 256 RGBA8 volume (U = geomagnetic azimuth, wrapping;
V = altitude over 40-410 km; W = colatitude over 0-75 deg), periodic gradient noise wrapping in U.

| Channel | Holds |
|---|---|
| R | the curtain folds (256 cells per turn of azimuth) |
| G, B | the column window's two height bounds (64 cells per turn) |
| A | unused |

Sampler: linear, repeat in U, clamp in V and W. It is binding 16 of the sky set and binding 8 of the cloud march
set. The only live noise call left per sample is the shimmer phase.

### The march

`auroraMarchCS()` marches the padded shell 40-410 km:

- **Legs.** The ray's path through the shell depends on where the eye is. From inside or above it, a ray that
  dips below the inner sphere has a **near leg** and a **far leg** (beyond the dip); both are marched
  (`auroraMarchSegment()`). Dropping the far leg halves the path near the horizon and the aurora looks cut off at
  a fixed altitude.
- **Terrain.** Both legs end at the scene depth, and a ray whose entry into the real shell is behind terrain
  skips the march.
- **Bounding test.** Five points along the path; if all are more than 80 deg from the nearer pole the ray is
  skipped. Most of the sky away from the poles costs nothing.
- **Steps.** Adaptive: \( N = \text{clamp}(\lfloor L / 15\,\text{km} \rfloor, 4, 64) \) per leg, evenly spaced.
- **Jitter.** The start is jittered per pixel by interleaved gradient noise and per frame by the TAA's Halton
  phase (`gAurJitter`), so the step pattern averages out in the sky's temporal AA instead of banding.

### Sheets

Over the diffuse volume, thin emissive curtains: the crisp, folded sheets of a real display.

- **Index field** (`auroraSheetIndex()`): \( S = (\theta - 20^\circ - 8^\circ\,\text{storm}) / \text{spacing} + W \),
  where \( W \) is a sum of travelling waves in azimuth (3, 11, 29 and 71 cycles per turn). The sheets are the
  level sets \( S = n \) for integer \( n \), spaced "Sheet spacing (deg)" apart. Where \( W \)'s gradient across
  the oval passes 1, a sheet folds back on itself into curls; "Sheet folds" scales the 11 and 29 cycle waves.
- **Exact integration per step.** With \( S \) taken as linear in \( t \) over a march step, each sheet's Gaussian
  profile (width \( \sigma \) in index units) integrates in closed form:

  \[ L = \Delta t\,\sigma\sqrt{\pi/2}\;\frac{|\operatorname{erf}(b) - \operatorname{erf}(a)|}{|\Delta S|}, \qquad
     a, b = \frac{S_{0,1} - n}{\sigma\sqrt 2} \]

  (Winitzki's erf, error below 1.3e-4). Edge-on curtains are therefore crisp ribbons at any resolution with no
  sampling noise. Candidate sheets are those within 1.2 sigma of the step's range of \( S \), at most four per
  step, faded at the edge of that window; on an actual crossing, two regula-falsi steps refine the exact index.
  The index is carried across every sample, including those where the oval mask is zero, so sheets are not cut
  into whole-step chunks.
- **Crisp or fuzzy** per sheet and per region: \( \sigma \) from 0.28 (fuzzy) to 0.03 (crisp) index units,
  with "Crisp sheets (share)" setting the mix.
- **Light along a sheet** (`auroraSheetLight()`):
    - a sharp **lower border** near 100 km (+-8 km per sheet, rippled), below which the light dies within
      about 2.5 km, coloured pink (N2), turning green within about 3 km above it;
    - above the border, an exponential fall-off with a 35-70 km scale height, turning red 80-150 km above the
      border;
    - **rays**: field-aligned striations, a sharpened sinusoid in azimuth (170-260 cycles per turn) wavering
      slowly with time. From under the oval they converge toward the magnetic zenith;
    - a per-sheet brightness from 0.25 to 1.75.
- The sheets' total is weighted by "Curtain sheets" and added to the volume's.

### Visibility

- **Brightness**: the march's sum x `kAuroraScale` (1e-6) x "Aurora gain".
- **Light pollution**: the aurora is treated as an extended source of a magnitude per area and dimmed against
  the sky background's (`darkSkySurfMag()`, `darkSkyVis()`), so a city's skyglow hides a faint aurora.
- **Moonlight**: dimmed by up to 85% under a high, full Moon.
- **From orbit**: those two gates are for a sky seen through the air; they fade out between 40 and 100 km of eye
  altitude, so from space the aurora shows against the night side.
- **Extinction**: the line-of-sight extinction (`atmExtinctionMag()`, [Atmosphere and sky](atmosphere-and-sky.md)).
  From below 95 km it is taken to infinity; from above, only to the aurora's inner sphere, since looking down the
  path to infinity runs into the ground.
- **Clouds**: from below 95 km, the aurora is hidden by the cloud transmittance in front of it (cubed).

The aurora puts no light on the ground or the sea surface: a real aurora lights the ground no more than a full
Moon does, and colourlessly to the eye; the sky's own light reaches the ground through the sky ambient.

### Other views

- **Environment probes and mirrors** (`envAurora()`): a 16-step midpoint march of the volume through the shell,
  with extinction, no sheets and no visibility gates.
- **In the sea**: a 6-step march of the volume along the reflected ray, with extinction, hidden behind reflected
  clouds and faded with the wave range ([The sea](sea.md#reflected-clouds)).

## Cost

The volume, the sheets and the red airglow run at half resolution. The noise bake and the bounding test keep the
aurora's cost small where there is none in view; the sheets add about 0.5 ms of cloud march in a storm view
(RTX 3070 Ti, harness). The red airglow march (16 fixed steps) would cost about 1 ms at full resolution, which is why it runs at half.

## Settings

Atmosphere tab ("Airglow & zodiacal light" and "Aurora" sections); all under `clouds` in `settings.json`.

| UI label | Key | Default |
|---|---|---|
| Airglow gain | `airglow_gain` | 0.066 |
| Airglow green | `airglow_green_gain` | 0.053 |
| Airglow red | `airglow_red_gain` | 0.013 |
| Airglow sodium | `airglow_sodium_gain` | 0.08 |
| Airglow coverage | `airglow_coverage_gain` | 0.32 |
| Airglow polar boost (red) | `airglow_polar_gain` | 2.4 |
| Storm strength | `storm_strength` | 0.33 |
| Aurora gain | `aurora_gain` | 0.1 |
| Coverage freq | `aurora_coverage_freq` | 0.43 |
| Coverage az freq | `aurora_coverage_az_freq` | 4.3 |
| Coverage drift | `aurora_coverage_drift_rate` | 0.0012 |
| Fold shimmer rate | `aurora_shimmer_rate` | 0.0018 |
| Curtain sheets | `aurora_sheets` | 1 |
| Sheet spacing (deg) | `aurora_sheet_spacing_deg` | 0.3 |
| Crisp sheets (share) | `aurora_sheet_crisp` | 0.5 |
| Sheet folds | `aurora_sheet_fold` | 1 |

Knockout bits: 32 the aurora march, 16 the red airglow march. The Low preset turns the aurora off; Planetarium and
Potato turn off both.

!!! tip "Testing"
    Test from a dark site under the oval in winter (for example Coldfoot, 67.25 N 150.18 W, in December). A
    city's skyglow gates the aurora out, so a site such as Fairbanks shows little.

## Where in the code

| File | What |
|---|---|
| `shaders/cloud_march.comp` | `auroraMarchCS()`, `auroraMarchSegment()`, `auroraCurtainSample()`, `auroraSheetIndex()`, `auroraSheetLight()`, `airglowRedMarchCS()` |
| `shaders/aurora_noise.comp` | the noise bake |
| `shaders/sat_sky.frag` | green and sodium airglow, `auroraSampleAt()`, `envAurora()`, the sea reflection |
| `shaders/include/cloud_params.glsl` | the aurora and airglow UBO fields |
| `src/simulations/SatelliteSim.cpp` | `createAuroraNoisePipeline()` |
