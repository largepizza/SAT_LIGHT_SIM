# Atmosphere and sky

`shaders/sat_sky.frag` draws one fullscreen triangle and produces the background of the frame: atmosphere,
terrain, sea, satellite meshes, clouds, Sun, Moon, Milky Way and zodiacal light, tonemapped. It is also the
**compositor**: it reads the results of every earlier pass and writes the unified depth that point sources are
tested against. This page follows `main()` in order. Terrain, sea, clouds, aurora and the Moon have their own
pages, and the Sun's disc, refraction and mirages are on [The Sun as an object](refraction.md); here they appear
only where the sky pass consumes them.

## Inputs

- **Push constant** `SatDrawPC` (exactly 128 bytes): the camera matrix, field of view, aspect, GMST, wave
  time, the Sun and Moon directions in observer ENU, and the observer's ECEF direction plus height offset.
- **`CloudParams` UBO** (binding 9): every other per-frame uniform, including the knockout mask, the sky
  render-target size, the exposure scale and the eclipse geometry.
- **Results of earlier passes:**

| Binding | From | Holds |
|---|---|---|
| 0 | `sat_flare.comp` | the 64-bin satellite sky-glow histogram |
| 10 / 11 | `cloud_march.comp` | `cloudTargetA` (cloud light, signed-km distance) / `cloudTargetB` (transmittance, ground shadow), ½ res |
| 19 | `scene_depth.comp` | `sceneDepthTex`, ½ res, metres |
| 20 | `sat_flare.comp` | the ocean-glint list |
| 21 | CPU | Reflect-beam ground spots, solar park sites, and the refraction table with the sunspots ([The Sun as an object](refraction.md)) |
| 22 / 23 | mesh scene pass | mesh radiance / distance, full res (`imageLoad`) |
| 26 | `scene_depth.comp` | `terrainFrame.x`, the eye's detailed ground height |
| 29 | `cloud_v2_far.comp` | the far cloud layer, full res (`imageLoad`) |
| 30 | cloud passes | the clouds' flash buffer: the Sun transmittance volume, the god-ray froxels, the multiple-scattering table \( \Psi \), the ground's sky-light table and the cloud cover map over the ground |
| 31 | CPU | the clouds v2 UBO (`clouds_v2.glsl` with `CV2_PARAMS_ONLY`) |
| 32 | CPU | the dust list: wildfire smoke (for the Sun's light on the ground) and, at its tail, the engine and floodlight list the sea mirrors |
| 33 | `sky_ap.comp` | the [aerial-perspective volume](#the-aerial-perspective-volume) |

"½ res" means half of the render-scaled extent (see [Resolution scaling](#resolution-scaling)). The
sampled-image bindings sit exactly at the Vulkan per-stage minimum of 16 (see
[Weak-hardware tiers](hardware-tiers.md#the-hardware-floor)), which is why the newer full-resolution inputs are
storage images read with `imageLoad`.

## The ray and the first surface

1. **View direction.** The interpolated, unnormalised ENU direction from `sat_sky.vert`. Under `SKY_TAA` it
   is offset by the frame's sub-pixel jitter through its screen derivatives, which are exact because the ray
   varies linearly across the fullscreen triangle.
2. **Eye.** `obsEffH = terrainFrame.x`, the same detailed ground every pass uses; the eye is 2 m above it.
3. **Terrain.** The full-resolution terrain march, seeded from `sceneDepthTex`, gives `tSurface`
   (see [Terrain](terrain.md)); a ray that misses falls through to the sea-level sphere and the water test
   (see [The sea](sea.md)).
4. **Mesh.** `tMesh` from the full-resolution mesh distance. If it is nearer than the terrain or sea,
   `tSurface = tMesh`: from here on the atmosphere loop, every disc gate and the depth stop at the mesh.

## Reading the clouds

The half-resolution cloud targets are read **before** the atmosphere loop, because the loop needs to know where
the cloud is. The cloud pipeline itself is on [The cloud march](clouds/march.md) and
[Temporal resolve and the far layer](clouds/temporal.md).

**Upsampling.** A bilinear tap at the pixel's UV. The clouds' temporal resolve already anti-aliases their
silhouettes, so nothing is blurred. Where a **surface** edge crosses the half-res grid (the log depth range
over the 2×2 texels exceeds 0.1), the colour is re-averaged joint-bilaterally:

\[
w_k = b_k \, \exp\!\left(-6\,\left|\ln d_k - \ln t_\text{surface}\right|\right)
\]

with \(b_k\) the bilinear weight and \(d_k\) the half-res scene depth of texel k. A ridge against cloud then
keeps the full-resolution silhouette instead of half-res stair-steps.

**Far cloud layer.** Above about 600 km of eye altitude the far layer is blended behind the half-res
composite by `farBlend`:

```glsl
cloudA.rgb += farBlend * L_far * cloudB.rgb;
cloudB.rgb *= mix(1.0, T_far, farBlend);
```

**Distances.** The target's alpha is a signed distance in **kilometres** (`include/cloud_occlusion.glsl`):
≥ 0 is an opaque cloud, < 0 the transmittance-weighted mean distance of translucent cloud.

- `tCloudOcclude` is the opaque distance (or −1). It gates the Sun and Moon discs and feeds the depth.
- `tAirFrontM`, the **air split**, is where the air "in front of" the cloud ends: the four gathered texels'
  distances weighted by bilinear share × opacity, ignoring texels with no real distance, falling back to the
  nearest real cloud. A far-layer share uses the far sphere's distance.
- `tCloudFrontM` is the nearest real cloud, for the city glow split.

The distance in the alpha already accounts for plumes. `cloud_march.comp` pulls it toward the light-weighted
distance of every plume in front of the cloud (a tornado, a haboob wall, wildfire smoke, launch exhaust that no
engine light reaches), by the plume's opacity there: an opaque funnel a few km away in front of rain or haze tens
of km out must not have all the horizon haze in between laid over it. The smoke's distance is that of the plumes in front of the cloud
(the opacity profile's first segment), never pulled nearer by the fog and dust haze in the resolved mean. Engine-lit
exhaust is left out: the engine-light pass would light it a second time as cloud. See
[Launch visuals](launch-visuals.md).

## The atmosphere

**Range.** From the ray's **entry** into the atmosphere (`tStart`, 0 from inside it) to
`min(atmosphere exit, tSurface)`. The atmosphere is a shell 100 km thick (`R_ATMOS`). Starting at the entry
matters from orbit, where nearly every step from the eye would fall in vacuum and the few left in the air would
undersample it.

**Two ways to integrate it.** Most pixels read the integral from the
[aerial-perspective volume](#the-aerial-perspective-volume), computed once per screen tile by a compute pass. Within a
band round the horizon and the Earth's limb, where the light changes too fast across a tile, the sky pass runs the
same integral itself, per pixel, as described in the rest of this section.

!!! warning "Invariant: one integral in two places"
    `sky_ap.comp` duplicates the per-pixel loop of `sat_sky.frag` (the step rule, the media, the shadows, the god
    rays, the eclipse, the city glow, the airglow). The two meet at the band's edge, so any change to one must be
    made to the other, or a step appears round every horizon.

### Steps

A march whose step length is held at about \(100\,\text{km}/N_\text{min}\):

\[
N_\text{VIEW} = \operatorname{clamp}\!\left(\left\lceil \frac{t_\text{end} - t_\text{start}}{100\,\text{km} / N_\text{min}} \right\rceil,\ N_\text{min},\ N_\text{max}\right)
\]

A grazing ray therefore gets more steps than a steep one from the same altitude, instead of a coarser step.

Each step's densities are **integrated over the step**, not sampled at its middle: for a height that is linear from
the step's start to its end, the mean of \( e^{-h/H} \) is exact (`atmHalfAvg3()`, one reciprocal for the three
columns), and the step that holds the ray's lowest point (a ray from altitude dipping toward the horizon) is split at
its middle. The step's light is attenuated by the camera column to the step's **middle**. With the default
\(N_\text{min}\) = 6.5 (steps of about 15 km looking up), a midpoint sample of the Mie (1.2 km scale height) or the
haze near clouds (2.5 km) would be off by up to fifty times, and since \(N_\text{VIEW}\) is a whole number that grows
with the path, the error would jump every few degrees from the zenith: concentric rings round the zenith. Integrated,
the sky has almost no step-count structure: the step count still changes, but the result does not.

The step's start height and its exponentials are carried from the previous step's end, so each step costs one
vector `exp`.

### Media and the Sun's path

Rayleigh with \(\beta_R = (5.8, 13.5, 33.1) \times 10^{-6}\,\text{m}^{-1}\), scale height 7994 m,
phase \(\tfrac34(1+\cos^2\theta)\). Mie with \(\beta_M = 2.1 \times 10^{-5}\,\text{m}^{-1}\), scale height 1200 m,
Cornette-Shanks phase with g = 0.26, and extinction 1.1 × scattering for aerosol absorption. Both coefficients are
multiplied by the "Rayleigh gain" and "Mie/haze gain" sliders.

**Haze near clouds.** Humid air under and around weather systems carries more aerosol. Below 12 km each step's Mie
column gets an extra term, "Haze near clouds" × (0.25 + 0.75 × the weather cube's cover + 0.6 × its rain), with a
2.5 km scale height, integrated over the step like the others. Aerosol scatters forward: with it a sunlit stretch
between cloud shadows glows toward the Sun as a ray, where clean air would show only darker bands.

**The Sun's path** to a sample is, with the multiple-scattering sky model (the default), exact: the Chapman Rayleigh
and Mie columns plus **ozone** (`sunPathTauExact()`, `include/sky_atmos.glsl`). Ozone is a uniform layer from 15 to
35 km with the Chappuis band's vertical optical depths 0.019 / 0.027 / 0.0016 at 680 / 550 / 440 nm, crossed for the
ray's exact chord (`atmOzoneTau()`, `include/atmosphere.glsl`). A thin shell at 25 km times a secant would be
infinite where a sample's Sun ray grazes it and draw a hard-edged ghost in the twilight sky; the chord through a
layer stays finite. Every copy of the ozone model (the clouds' Sun table, the far cloud layer, the lightning pass)
uses the same layer. With the single-scattering model the Sun's column comes from `optDepth()`, a midpoint march of
"Light samples" steps without ozone.

### Per step

With \(\tau_\text{cam}\) the column back to the eye:

- **Sunlit sample** (the Sun is not behind the Earth from it):
  \( a = \exp(-[\beta_R(\tau^R_\text{cam} + \tau^R_\text{sun}) + 1.1\beta_M(\tau^M_\text{cam} + \tau^M_\text{sun})] - \tau_{O_3}) \),
  (the ozone term with the multiple-scattering model only), accumulated into Rayleigh and Mie in-scatter weighted
  by the step's integrated densities, then multiplied by:
    - the **orbital terminator gate**, above 40 km of eye altitude only, weighted by the sample's own
      geographic Sun elevation. It is a deliberate tone-mapping correction rather than physics: the twilight
      tail of the scattering integral is correct but survives the tonemap far more visibly than in orbital
      photographs. Daylit samples are untouched.
    - the **god rays**: the shadow of clouds and terrain on the air (below);
    - the **cloud-hidden Sun**, only while the god rays do not already shade the air: `cloud.sunCloudT` (the eye's
      transmittance toward the Sun) for air below 8-14 km, within about 50 km along the ray and about 3 km of the
      eye's line to the Sun. One cumulus in front of the Sun must not darken the whole sky, but under a storm the
      haze's forward peak must not draw a bright Sun-shaped glow where the disc is hidden;
    - the **eclipse**: see [Moon and eclipses](moon-and-eclipses.md#the-sky-in-the-moons-shadow).
- **The Earth's shadow, exactly.** Once per pixel the shader solves the interval of the view ray that lies inside
  the Earth's shadow cylinder (`tShA`, `tShB`, a quadratic), and each step takes the **sunlit share** of its
  length. Its Sun path is taken from the middle of the lit part. A test at each step's midpoint would make a step
  of 10-15 km lit or shadowed as a block, and near the horizon the first step holds almost all the haze, so at the
  elevation where its midpoint left the shadow the sky would change brightness and colour in a hard band, laid over
  every cloud in it through the air split. The same exact share (`earthShadowSpan()`, `earthShadowLit()`) is used by
  every other Earth-shadow integral in the shader: the zenith sky light (`skyZenithSky()`), the flat layers'
  twilight and the sea's sky reflection.
- **Multiple scattering** (the default sky model): each step adds
  \( (\beta_R \rho_R + \beta_M \rho_M)\, \Psi_\text{ms}(h, \mu_\odot) \) attenuated by the camera column, where
  \(\Psi_\text{ms}\) is Hillaire's (2020) multiple-scattering function: the light that sunlit air at height \(h\)
  and Sun cosine \(\mu_\odot\) receives from all orders of scattering, per unit of scattering. It is the 32 × 32
  table the clouds' sky-light bake already makes (`cloud_v2_sunlut.comp`, ground albedo "Ground albedo (sky light)"),
  read from the flash buffer by `cv2SkyPsiAt()`. This is what lights the air in the Earth's and the clouds' shadow
  and turns the twilight zenith blue-violet; during a solar eclipse it takes the eclipse's direct share and sky light.
- **Single-scattering model**: the shadowed share is accumulated as *shadowed air* instead, and after the loop it is
  lit as isotropic in-scatter of half the zenith sky's radiance at its density-weighted mean point
  (`skyZenithSky()`), faded over the first few degrees of Sun depression there; off from orbit, where the terminator
  gate owns the twilight air. Without it the air beyond the terminator is black and the horizon facing away from a
  just-set Sun reads as a dark band.
- **City glow**: the night map under the sample, attenuated toward the eye. From the ground and aircraft the
  map is read at mip 4 (about 80 km texels): a horizon glow gathers the light of a whole region. Air at height
  h is lit by the cities within a few h, so seen from high orbit (blended in over 800-3000 km of eye altitude)
  the mip follows the sample's height, \( \log_2(3h / 4.9\ \text{km}) \) clamped to 0-4 (about 1.5 at the
  8 km scale height): a town's glow is a halo the shape of the town rather than a disc some 150 km wide. Below
  800 km a 10-20 km texel spans tens of pixels, and bilinear filtering would draw it as squares. Skipped while the
  composite would multiply it by 0 (by day).
- **Airglow**: the green (96 km) and sodium (90 km) bands, gated by the sample's own geographic night, with their
  noise evaluated only between 62 and 130 km, where the profiles are not negligible. The red band is marched at half
  resolution in `cloud_march.comp`. See [Aurora and airglow](aurora-airglow.md).
- **The piece in front of the clouds.** The step the air split falls in is cut there. The piece in front takes its
  own integrated density, its own share of the Earth's shadow and of the clouds' shadow (four god-ray reads of its
  own), and reaches the eye through the air to the piece's own middle, not the step's. It goes to separate "front"
  accumulators, as do whole steps in front of the split; the city glow in front of the nearest cloud goes to
  `accumCityFront`. A step below the horizon is about 16 km long and one just above it up to 24 km, so a piece that
  took the whole step's light or attenuation would be wrong by kilometres of low, hazy air: a light band along the
  horizon across shaded clouds, or a dark line through a wall or funnel a few km away.

After the loop:

```glsl
color    = SUN * (pR*βR*accumR + pM*βM*accumM) + multiple scattering (or the shadowed-air term) + city dome + airglow + ...
airFront = SUN * (pR*βR*accumRF + pM*βM*accumMF) + the same terms for the front pieces
cityGlowFront = (city glow in front of the nearest cloud) * smoothstep(40 km, 150 km, tCloudFront)
```

`color` holds the air along the **whole** path to the surface, including the stretch in front of any cloud.

### God rays: shadows on the air

The air between the clouds is shaded by the same Sun shadow the clouds themselves take, so clouds and the air
around them share one light. The shadow is the clouds' **Sun transmittance volume** (four nested lattices of
parallel rays toward the Sun, 250 m to 16 km cells, see [The cloud march](clouds/march.md)) times the terrain's own
Sun-horizon map ("Terrain god rays"). The sky pass reads it in the full variants (`SKY_SUN_SHADOW`: every variant
except `SKY_LITE` and the `SKY_ENV` probes and mirrors).

**The froxel volume.** Rather than reading the lattices directly at every step of every pixel, the sky pass reads a
froxel volume (`cloud_v2_froxel.comp`, "God-ray froxels", on by default): 320 × 180 screen tiles × 64 depths spaced
logarithmically from 50 m to 400 km, filled each frame from the transmittance volume. Each froxel samples its own
point in its cell, stepped each frame along an R-sequence (`cv2FroxJitter()`, two depths a froxel), and the sky pass
reconstructs trilinearly from those same points (`cv2GodFroxT()`), so the [sky TAA](#sky-taa) averages successive
frames into a smooth tent over each tile: shaft edges have no staircase and thin shafts do not alias. Without the
sky TAA the samples sit at the cells' centres (four depths a froxel). With the froxels off the sky pass reads the
transmittance volume and the terrain map directly (`cv2StvTFine()`, `cv2TerrSunTFine()`): exact per pixel, about
20-25 ms a frame more at 1920 × 1009, which is what the HQ photo uses while it settles.

Per atmosphere step:

1. **Skip where nothing shades.** A step whose lowest point is more than 3 km above the transmittance volume's top
   (and the terrain ceiling, 9.4 km), under a Sun above the sample's horizon, is in no shadow; its reads all return
   1 and are skipped. Most of a horizon ray's steps run up there.
2. **Reads.** "God ray reads" points along the step within 30 km of the eye (8 by default), half that beyond, at a
   per-pixel and per-frame phase. Each read is weighted twice, by how the Rayleigh (7994 m) and the Mie and haze
   (1500 m) columns thin along the step with height, giving one shadow share for each, so a step's shadow sits where
   its air is. The weights are ray constants, computed once per pixel.
3. **Shading.** The step's sunlight is scaled by \( 1 - k\,(1 - T) \), with \(k\) = "God rays" (0 to 1, how much of
   the shadow the air takes). The shadowed air is lit by the sky like the air in the Earth's shadow (by multiple
   scattering, or the shadowed-air term), so a storm's shadow is a blue-grey wedge, not a black one. The piece in
   front of the clouds takes its own four reads in the same loop, so the haze in front of a cloud is shadowed like
   the sky beside it.

Looking down-Sun at sunset the shadows converge on the antisolar point (anticrepuscular rays).

### The aerial-perspective volume

`shaders/sky_ap.comp` runs the atmosphere loop above **once per screen tile** instead of once per pixel, and stores
its running sums at 48 depths along each tile's ray (`include/sky_ap.glsl`):

- **Grid.** The god-ray froxels' tiles (320 × 180: about 6 pixels at 1920 wide) with their per-frame sample offset, so
  the sky TAA averages the reconstruction. Depths are a **normalised** distance \( u = (t - t_\text{start}) / L \)
  along the ray's path through the air (to the atmosphere's exit or the sea-level sphere), spaced logarithmically
  from **both** ends (200 m from each end): fine near the eye for near surfaces, and fine near the end, where a ray
  descending into dense air (from orbit, from an aircraft) gathers most of its light. A pixel maps its own distance
  through its own ray's \(t_\text{start}\) and \(L\), so a sky pixel reads the last slice exactly.
- **What is stored.** Per cell 12 half floats: the Rayleigh and Mie in-scatter, the light of the air in shadow
  (multiple scattering or the shadowed-air term), the city glow, the camera's Rayleigh and Mie columns, and the
  green and sodium airglow columns. The in-scatter, glow and columns are stored **per unit of the ray's own air
  column** to that depth (`skyApNorm()`, the Chapman column of each species) and multiplied back by the pixel's own.
  Raw sums grow like the path (as 1 / cos of the incidence near the limb), and a tile's bilinear blend of that
  convex growth reads brighter than the pixel; per unit of air they vary slowly. The shadowed and multiply scattered
  light is stored × 10⁴, since per unit of air it lies below a half float's normal range and would quantise to
  contours.
- **The pass.** One thread a tile, after the froxel pass. Its steps follow the slices, cut into sub-steps not longer
  than the pixel loop's step; the god rays read the tile's own froxel column; the shadowed air's sky light is taken
  at the shadow's mean point up to each slice. Cost about 0.3-0.7 ms on an RTX 3070 Ti at 1920 × 1009.
- **The read.** The sky pass reads the sums trilinearly at the pixel's distance, at the air split (for the air in
  front of the clouds) and at the nearest cloud (for the city glow's split).
- **The band.** Within "Aerial perspective horizon band" (2°, at least 2.5 tiles) of the horizon or the Earth's limb
  (and of the atmosphere's top seen from above it) the per-pixel loop runs. Over the band's outer part, from 2.5 tiles
  out to its edge, the pixel computes both and cross-fades. The volume is the more accurate of the two (its slices
  are finer than the loop's default steps), so a hard edge would be a visible step. Every term mixes across the
  cross-fade, the shadowed air's sky light included.
- **Off** above 1000 km of eye altitude (the loop then starts at the top of the atmosphere and is cheap; the pass
  would cost more than it saves), on the light tiers, in the environment probes and mirrors, and in the HQ photo,
  which runs the exact loop in every pixel. "Aerial perspective volume" at 0 runs the loop everywhere.

## Surfaces and pre-tonemap terms

In order of `main()`:

- **The Moon's disc**, drawn at its true topocentric position and size and occluded by any surface or opaque
  cloud. See [Moon and eclipses](moon-and-eclipses.md).
- **Satellite sky glow.** The 64 bins of `sat_flare.comp`'s histogram (8 azimuth sectors × 8 elevation bands)
  each hold the **summed** displayed effectFlare of the bin's flares brighter than 20 (about magnitude −0.8). A
  bin's response is \( \operatorname{clamp}(\log_2(\max(F/300, 1))/4,\ 0,\ 1.5) \): it starts at a summed
  magnitude of about −4.4 and saturates 64 times past it. A lit Reflect site (about 150 mirrors at magnitude −3
  in one bin, summed about 6000) reaches about 1.1, while one tumbling piece of debris at −4 gives nothing.
  The bins are drawn as wide Gaussians (σ = 0.9 rad). Each is weighted by
  the **smaller** of two air columns: along the pixel's ray, and between the eye and the bin's direction (from
  the eye's height, or the tangent height of a downward path, Chapman airmass capped at about 35). The glow is
  the satellite's light scattered by that air; weighting by the pixel's ray alone would light the whole limb
  around every bright satellite seen from 40 km up. Knockout bit 65536 skips the loop.
- **The surface**: land or sea shading × \(\exp(-\tau_\text{cam})\). A mesh pixel adds the mesh radiance × the
  same transmittance. Reflect-beam ground spots are part of the surface shading.
- **Flat 2D cloud layers** (`evalCloudLayer()`). In the main view these draw only when the volumetric march is
  knocked out (bit 32768). They are the clouds of environment probes, sharp reflections, SKY_LITE and Potato,
  read from the same evolving weather cube the volumetric clouds are built from.

## The cloud composite

```glsl
if (meshHit && abs(cloudA.a) * 1000.0 > tMesh) { cloudB.rgb = vec3(1.0); cloudA.rgb = vec3(0.0); }
color = color * cloudB.rgb + cloudA.rgb + (cityGlowFront + airFront) * (1.0 - cloudB.rgb);
```

Reading it:

- `color · B` attenuates everything behind the cloud: the surface, and the air **including** the stretch in
  front of the cloud.
- `airFront · (1 − B)` puts that front stretch back, so it ends up unattenuated, as it should be.
- `cloudA.rgb` is the cloud's own light, already attenuated by the air in front of it, plus everything else
  `cloud_march.comp` carries: aurora, red airglow, lightning, rain drops, beam shafts and lines.

**Why split the air this way.** The cloud march outputs no airlight of its own. The air in front of a cloud is
the sky pass's own integral, so a cloud on the horizon sits in exactly the same haze as the clear sky beside
it. A separate airlight in the march reads darker than the sky's at a grazing Sun and turns horizon clouds into
dark silhouettes. The city glow in front of a cloud is kept out of its attenuation for the same reason;
otherwise night clouds along a lit horizon form a black band.

**The mesh test.** The march clamps only to the half-resolution scene depth, which misses a mesh's thin parts
(panels, trusses), and the far layer has no depth test at all. A mesh pixel whose cloud is farther than the
mesh skips the composite.

## Exposure

**The sky's exposure.**

\[
E = \operatorname{mix}(10,\ 1.8,\ \text{dayness}^{0.4}) \cdot 2^{\,\text{EV}},
\qquad \text{EV} = \text{"Exposure (EV)"} + \text{EV}_\text{auto}
\]

with dayness \(= \operatorname{clamp}((\sin \text{el}_\odot + 0.2)/1.2)\). During a solar eclipse dayness is
also multiplied by the share of sky light left at the observer (`moonEclipseSkyObs`, carried as
`cloud.moonMisc.y` = 1 + that share while an eclipse is possible), so the eye adapts as it does at twilight:
totality is shown at about twilight's exposure and the stars come out. The factor \(2^{\text{EV}}\) is the
**global exposure** (`globalExposureEV()`, uploaded as `cloud.exposureScale`). It also scales the point sources
(their reference and limit magnitudes shift by \(2.5 \log_{10} 2^{\text{EV}}\), so stars and satellites dim with
the sky) and every display-space term below. `SatelliteSim::skyExposure()` is the CPU mirror, eclipse term
included, used by the mesh bloom threshold, the environment probes and the post-tonemap terms, so the sky pass
and every other consumer agree on the exposure in and out of an eclipse.

**Auto exposure** (`readExposureMeter()`) is a closed loop on the **displayed** frame:

1. `recordScreenshotCopy()` blits the central 60 % of the presented image to 64 × 36 every frame while auto
   exposure is on; the CPU reads it the next frame.
2. **Near the ground** it steps the offset toward a linear mean luminance of 0.32, darkening harder when more
   than 4 % of pixels clip. It only darkens (down to −3 EV × "Auto exposure (day)") and only by day: full
   control above about 6° of Sun, easing back to 0 at night.
3. **From orbit** (blended in from 100 to 1500 km) it becomes a **spot meter on the lit Earth**: the mean of
   pixels brighter than 0.04, toward 0.47, with clipping counted against the lit pixels only, down to −5 EV,
   gated by lit Earth in view rather than the Sun at the observer. A whole-frame mean counts black space and
   would never darken a small bright Earth.
4. It eases with τ = 0.8 s when darkening and 1.6 s when brightening.

The 0.47 target and the orbit grade below were fitted to Artemis II photographs of the Earth, in which sunlit
cloud is about ten times the open sea in linear light.

## Tonemap and colour

In order, at the end of `main()`:

1. **White balance.** The colour is divided by the colour of sunlight at the observer (the Chapman column of
   both species, luminance-normalised, clamped to 0.4-2.5 per channel), scaled by "White balance" and faded
   out between 11.5° and 1° of Sun. A low Sun's colour is the look; adaptation is for a Sun well up, where
   without it every cloud reads beige. Near the horizon the sunlight's colour swings with altitude, and
   dividing by it would flip the same clouds gold to white as the observer climbs.
2. **Tonemap: AgX** (Troy Sobotka's, in Benjamin Wrensch's polynomial fit; `skyAgX()`). With \(x = E \cdot c\),
   the colour is taken into an inset gamut (a matrix pulling the primaries inward), mapped by a log2 curve over
   −12.5 to +4 EV about mid grey through a sixth-order polynomial, given a **look** (a power on the curve's output
   for contrast, "AgX contrast"; saturation about the luminance, "AgX saturation"), and taken back out of the inset
   gamut to display-linear for the sRGB swapchain. Highlights roll off to white **keeping their hue**. A
   per-channel curve saturates each channel on its own, so bright orange skews yellow and sunlit cloud flattens to
   paper white. Mid grey 0.18 maps to about 0.18, so the exposure keeps its meaning. A low Sun's disc stays a disc
   in its glow, and a twilight band goes pinker rather than clipping to yellow.

   With "Tonemap" at 0 the curve is the per-channel exponential
   \[ c' = \operatorname{mix}\!\left(1 - e^{-x},\ 1 - \frac{1}{1 + x + x^2/2},\ r\right) \]
   where r is "Highlight roll-off" (read only by this curve). The second curve equals the first to second order
   (same toe and midtones) but has a long shoulder. `SKY_LITE` and Potato always use this curve; the `SKY_ENV`
   probes output HDR and are tonemapped by the view that shows them.
3. **Orbit colour grade.** Weighted by "Orbit colour grade" × a ramp from 30 to 300 km of eye altitude, and
   per pixel by the Sun's elevation at the surface point (or the limb's tangent point), from −6° to +3°:
   \( g = \min(1.16\,c'^{2.2}, 1) \), desaturated 30 %. It applies only on the sunlit Earth: on the night side
   its power curve would crush city light, moonlit cloud, airglow and aurora about tenfold.
4. **Night floor**: a faint bluish lift, `(0.0008, 0.001, 0.002)` × nightness.

## Display-space terms

These are added after the tonemap and multiplied by `cloud.exposureScale`, so they read at a consistent
brightness whatever exposure the HDR scene needed that frame.

### Milky Way

The panorama `milkyWayTex` is read in the galactic frame (`cloud.mwBasisRow*`). Its visibility is the product
of:

- night and the CPU-eased Sun glare gate (`cloud.skyGlareVisibility`), blended toward "no sky" by the eye's
  altitude between 40 and 100 km;
- the **dark-sky gate** (`include/darksky.glsl`): the sky background's surface brightness in this direction
  (light pollution dome, twilight) against this texel's own surface brightness. Because the gate is per texel,
  the faint outer arms drop out as the sky brightens while the core hangs on: a suburban sky makes the Milky
  Way thinner, not uniformly dimmer;
- moonlight (up to 95 %) and a nearby Reflect beam's sky glow (up to 99 %);
- line-of-sight extinction (`atmExtinctionMag()`, shared with satellites and stars);
- the Sun's glare: zero within about 7° of a Sun above the horizon, full beyond about 29°;
- not behind a surface or the Moon's disc;
- cloud transmittance cubed (a "mostly opaque" 0.25 must hide it).

### Zodiacal light and gegenschein

An analytic cone in the ecliptic: an inner fade clear of the solar corona (0.09-0.17 rad), an outer fade at
"zodiacal outer fade" (80°), a Gaussian in ecliptic latitude whose width ("Zodiacal width") narrows with
elongation, plus a dim gegenschein patch (7 % of the cone) opposite the Sun. Its colour is a pale warm white.
Its visibility uses the same gates as the Milky Way except the Sun glare, through the same dark-sky model with
its own surface-brightness anchor (0.01 ↦ 21.6 mag/arcsec²), so it drops out one step earlier on a brightening
sky, as in reality. Cloud transmittance squared.

### Moonlight, Moon glow

A faint moonlit ambient (× the Moon's elevation and light, × the air column), a tight corona at the disc and a
wide diffuse halo, both scaled by the air the ray crosses so they vanish above the atmosphere.

### Sun disc, glare and corona

Drawn when the Sun is above the limb (`limbZ`, the sine of the horizon's depression from the eye's altitude). In
the full variants the disc is drawn through the refraction table: flattened near the horizon, with mirages, a green
rim and a boiling limb, at its physical radiance through each disc pixel's bent air column and the cloud composite,
with limb darkening and sunspots. Three stacked glare lobes and a wide corona follow the light of the visible disc.
All of that is on [The Sun as an object](refraction.md). The disc and its veil are hidden by any surface or opaque
cloud on the pixel's own ray (`sunGate`) and scaled by `min(cloud transmittance, cloud.sunCloudT)`.

Near the Moon the disc takes its true radius and is hidden under the Moon's disc, the glare and corona follow
the visible share of the Sun, and at totality the solar corona is drawn. See
[Moon and eclipses](moon-and-eclipses.md#solar-eclipses).

### Lens flare

`lensFlare()` draws ghosts and a starburst for the **Sun only**, in screen space. Its visibility is tested at
the Sun's own screen position, not the pixel's: the cloud transmittance there, `cloud.sunCloudT`, the half-res
scene depth (a ridge hides it), and the share of the Sun seen past the Moon.

The large bokeh ghost is a disc reflected through the screen centre, drawn as three offset colour copies (the
chromatic fringe), each with the profile
\( \left(1/(1 + 32 r^2)\right)^3 \), the cube of a Lorentzian in the distance \( r \) from the ghost's centre. A
plain Lorentzian is still at half brightness 0.18 of the screen height out, so with the Sun in view from orbit its
tail lays a brown wash of about 0.05 (linear) over half the frame, enough to hide a dim earthlit station in front of
it; cubed, a ghost is a disc with an edge. Ghosts fade out as the Sun nears the screen centre
(`smoothstep(0.03, 0.12, |pos|)`). Satellites have no lens flare;
their glow is the bloom and glare of [Points, bloom and glare](points-bloom-glare.md).

## Depth output

```
t = terrain hit, else sea / far-land hit, else opaque cloud distance;  t = min(t, tMesh);  else the Moon's distance
gl_FragDepth = t >= 0 ? sceneDepthFromDistance(t) : 1.0
```

`SKY_TAA` writes the same value to an R32F colour copy; `SKY_ENV` has no depth attachment. The encoding and
the rule it serves are on the [Rendering overview](index.md#two-depth-representations).

## Sky TAA

Temporal anti-aliasing of the whole background (terrain silhouettes, detail normals, far textures, city
glitter, the sea), all of which alias because the sky pass shades one ray per pixel. Below 100 % render scale
the same pass is a **temporal upscaler** (see [Resolution scaling](#resolution-scaling)). It runs when
`skyTaaWanted()` holds: "Temporal anti-aliasing" on, the swapchain supports transfer-destination use, and
neither weak-hardware sky tier is selected. The render scale does not enter the test.

```mermaid
flowchart LR
  A["sat_sky.frag -DSKY_TAA<br/>Halton jitter<br/>RGBA16F + R32F depth"] --> B["sky_taa.comp<br/>reproject, reject,<br/>clip, blend"]
  H[("history<br/>ping-pong")] --> B --> H
  B --> C["blit to swapchain"] --> D["taa_depth_restore.frag<br/>opens the main pass"]
  D --> E["points, stars, planets<br/>drawn unjittered"]
```

1. **Render.** The background renders offscreen with a Halton (2, 3) sub-pixel jitter (`cloud.taaJitter.xy`,
   in input pixels) into an RGBA16F colour target and an R32F copy of its depth. The cycle is 8 phases at 100 %
   and 16 when upscaling, since each output pixel then needs more input positions. The input images are
   full-size; below 100 % the pass draws only their top-left `skyTaaInExtent()` through a dynamic viewport, so a
   change of scale recreates nothing.
2. **Resolve** (`sky_taa.comp`, 16 × 16 workgroups, one thread per **output** pixel):
    - **This frame's estimate and neighbourhood.** At 100 % the estimate is the pixel's own sample. When
      upscaling it is a Gaussian (σ = 0.6 output pixels) of the 3 × 3 input samples around the pixel, each at
      its jittered position. Either way the 3 × 3 samples give the YCoCg mean and standard deviation, and the
      mean luma **without the brightest sample** for the flash rule below. Non-finite samples are dropped (read
      as black): one NaN would otherwise spread through the history's Catmull-Rom taps into a growing block.
    - **Reprojection**, exact for a static world: the pixel's *unjittered* ray, its distance from the depth,
      the eye's motion and last frame's camera, all computed on the CPU in double in this frame's ENU. Sky
      pixels (depth 1) reproject by rotation alone.
    - **Disocclusion**: the depth the point should have had must lie within the **range** of the 2 × 2
      history depths around its old position, ± 0.004. At a silhouette the jittered samples alternate between
      sky and ground, so a test against a single texel would reject every edge pixel every frame and
      anti-alias nothing. When nothing moved this frame (below) the range also takes in this frame's 3 × 3
      depths, which hold both surfaces of an edge: a pixel on a crest seen at a low angle hits the ridge in some
      jitter phases and whatever lies beyond it in others, and last frame's 2 × 2 need not hold the depth this
      phase hit. In motion the range is the history's alone; widened by the current spread it would let history
      ghost through cloud edges while climbing.
    - **History**: a 9-tap Catmull-Rom read (bilinear alone blurs a little more every frame).
    - **Variance clip** toward the mean, a box of 1.25 σ, so anything that moves on its own (waves, clouds,
      beams, twinkling lights) cannot ghost. It is skipped when nothing moved (below).
    - **Weight**: the new sample's weight goes from "still" (0.1) to "moving" (0.35) by the larger of two
      ramps: the total reprojection motion over 0.5-4 px, and the **parallax** over 0.1-1 px. Parallax is the
      reprojection's offset from where a pure rotation would put the pixel, i.e. the eye's own movement. A
      rotation reprojects exactly, so it can keep the long history; but every frame of eye motion resamples
      the history at a fractional offset, and at the still weight about ten frames of that resampling stack
      up. Climbing or descending over a city at night the ground moves only 0.1-0.5 px a frame, and without
      the parallax ramp the pixel-sized lights smear into blotches. When the neighbourhood's mean brightens past the history (lightning, a sprite) the
      new frame is taken at once; at weight 0.1 a flash a few frames long would come out about ten times
      dimmer. When upscaling, the weight is then multiplied by the Gaussian weight of the nearest input sample:
      a sample that landed far from the pixel moves it little, and over the 16 phases every output pixel
      converges to its own value.
    - **Validity**: history is dropped on a change of field of view or aspect, an eye jump over 20 km, a
      cinematic cut, or a frame that did not use TAA.
3. **Blit** the resolved, full-size image into the swapchain.
4. **Depth restore.** The main pass (`renderPassLoad`) opens with `taa_depth_restore.frag`, which writes the
   resolve's full-resolution depth back with colour writes off. Stars, planets and satellites then draw
   unjittered, sharp and depth-tested over the anti-aliased background, at any render scale.

**When nothing moved.** The CPU tells the resolve through `eyeDelta.w` whether the whole camera is stationary:
the eye moved less than 1 cm, the view did not turn, and sim time has advanced by at most 1.5 times the frame's
duration since the last resolve (w = 2, or 3 when sim time has not changed at all; 1 otherwise, 0 without
history). Both tests read the sim time itself rather than the pause flag or the time scale: a cinematic or a
harness `path play` holds the pause flag while it sets the sim time every frame, and that is motion. When still,
the history is **not clipped**, the disocclusion range takes in this frame's depths, and the flash rule compares
against the mean without the brightest sample. A light smaller than a pixel is caught by only
some jitter phases; clipped against an otherwise dark 3 × 3 box, its accumulated light would be thrown away in the
other phases and it would blink on the jitter cycle. A lone glint on a wave facet does not raise the robust mean,
so it does not count as a flash. The decision is per frame, not per pixel: judged from each pixel's own
reprojection motion, slowly moving distant clouds would count as still while the camera climbs, and leave
trails. In motion the resolve behaves as in the steps above.

**The sea.** `sat_sky.frag` marks sea pixels in the colour target's alpha (0.5 × (1 − trail share); 1 elsewhere).
The waves move while the camera is still, so the sea keeps its clip when nothing moved and drops it only when
sim time has not changed since the last resolve (w = 3), when the waves are frozen too. On a rough sea where the
weather map rains, the **trail share** ("Storm sea trails" × a ramp on the sea state × a ramp on the rain there,
× the share not covered by cloud in front) deliberately keeps an unclipped history: the clip is blended toward
none, the weight is held at the still weight, and the flash rule leans toward the robust mean. The crests then
smear into trails like blown spray. The share fades out over 1-4 px of parallax a frame, so in flight the history
does not drag the clouds above the sea across the water. "Storm sea trails" at 0 turns this off. See
[The sea](sea.md#storm-sea-trails).

Two more marks on the sea. A **Sun-glint sparkle** the sea drew on purpose (alpha 0.6) is taken whole rather than
averaged into the history, where a lone flash would come out about ten times dimmer. Any other lone sea spark while
the eye moves (a glint on a sub-pixel wave facet caught by one jitter phase) is capped at the 3 × 3's second-largest
luma, since in motion the history does not average it away.

**Things that move on their own.** Reprojection assumes a static world, and satellite meshes and plume or flame
light have no motion vectors. `sat_sky.frag` marks them (alpha 0.875: a mesh pixel, or a composite texel flagged as
emissive), and the resolve stores their depth negated so the next frame knows where they were
(`taa_depth_restore.frag` takes the absolute value). Wherever the 3 × 3 or the history's 2 × 2 holds one, the still
view's exemptions (no clip, the widened depth test) are off, so a moving rocket leaves no trail.

**Unjittered city lights.** City lights are pixel-sized points that their inputs switch on and off (the night map
near zero at a city's fringe, the terrain contour, the glitter pattern), so under the jitter a light could be lit
in only one or two phases. Where the unjittered ray meets the ground at more than about 12° (\(|\cos| > 0.2\)),
the city lights read the night map, the terrain contour and their own pattern at the **unjittered** pixel's point
on the hit's tangent plane (`uvTaa0`, `taaDn`). Toward a grazing horizon that point slides kilometres for half a
pixel, so there the lights stay jittered. When upscaling, glitter points are at least one input pixel wide, so
every jitter phase sees each point.

The half-resolution cloud composite is inside this image, so cloud edges are averaged on screen too. Measured
cost at 100 %: about +1.1 ms still and +0.8 ms panning at 1600 × 900. The upscaler costs about 1 ms more than a
plain stretch.

!!! warning "Invariant"
    The main render pass's three variants (clear, load, boot) share one dependency list
    (`mainPassDependencies`). Render-pass compatibility includes dependencies; if they diverge, every draw of a
    load or boot frame is invalid against framebuffers and pipelines made with the clear pass. Run with
    `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` after touching any of this.

## Resolution scaling

"Render scale" (50 %-100 %, Display tab) shrinks the background, not the point sources: satellites, stars,
planets, city light sprites, satellite meshes and the UI always render at native resolution. What it scales,
how the presets set it, the measured gains and the **automatic render scale** (`updateDynamicResolution()`,
off by default: 5 % steps that keep the GPU frame under 0.9 × the target frame time) are on
[Weak-hardware tiers](hardware-tiers.md#render-scale). For the sky pass:

- **The compute passes can follow it.** With "Clouds follow render scale" on (off by default; the Low, Medium,
  Planetarium and Potato presets turn it on), `scene_depth.comp`,
  the cloud march targets and the clouds' screen images are sized by `computeHalfExtent()`, half of the
  **scaled** extent, and rebuilt by `recreateComputeScaledTargets()` when it changes. The cloud march keeps
  filtering its noise at the 100 % pixel, so the clouds keep their texture, only softer. The sky pass reads these
  targets by UV or by their own size, so it needs nothing extra.
- **With the sky TAA** the background is upscaled temporally ([Sky TAA](#sky-taa)). `cloud.skyScreenW/H` is the
  input extent the pass renders into, while its detail level (terrain detail and textures, city patterns, the
  pixel footprint) follows the **output** pixel, `cloud.skyLodScreenH`, so most of the full-resolution detail
  survives.
- **Without it** (TAA switched off, or SKY_LITE and Potato), `recordPrePass()` renders the background into an
  offscreen target of the scaled size and blits it (filter chosen per format by
  `VulkanContext::bestBlitFilter()`, since linear blit filtering is an optional feature) into the swapchain
  before the main pass, which then uses `renderPassLoad` so the blit survives. Depth is not blitted
  (depth-format blits are not guaranteed), so the point draws do a manual test against the half-res
  `sceneDepthImg` (`PointDrawPC::manualTerrainTest`) and occlusion by terrain is half-resolution on this path.
- **`gl_FragCoord` is relative to the current target.** Any normalised UV in `sat_sky.frag` derived from
  `gl_FragCoord` divides by `cloud.skyScreenW/H` (the size of whatever target this draw renders into), never
  by an assumed full-resolution constant.

## Shader variants

`sat_sky.frag` is compiled several times with defines (`CMakeLists.txt`):

| SPIR-V | Defines | Use |
|---|---|---|
| `sat_sky.frag.spv` | none | the main view without TAA, and the plain low-res prepass below 100 % render scale |
| `sat_sky_taa.frag.spv` | `SKY_TAA` | the main view with temporal AA or upscaling: jittered, and writes the R32F depth copy |
| `sat_sky_lite.frag.spv` | `SKY_LITE` | the Planetarium tier: see [Weak-hardware tiers](hardware-tiers.md) |
| `sat_sky_env.frag.spv` | `SKY_ENV` | environment-probe cube faces and the model viewer's background, rendered from a satellite's position |
| `sat_sky_refl.frag.spv` | `SKY_ENV SKY_REFL` | sharp mirror reflections, per mesh instance |
| `sat_sky_minimal.frag.spv` | (own file) | the Potato tier |

**SKY_ENV** renders the sky from another position (`pc.obsECEFDir` = the satellite's direction, w its
altitude). Everything screen-space is cut: the half-res cloud, depth and mesh targets, the sky-glow bins, the
ocean glints, the beam ground spots. The flat cloud layers draw at full weight, the aurora gets a short march
of its own, the Sun disc and lens flare are left out, and the output is pre-exposure HDR. The display-space
terms (Milky Way, zodiacal light, stars) follow the rules of **the view that will show the result**: they are
divided back by that view's exposure and gated by its Sun glare, both packed into `sunDirENU.w`. Stars are drawn
by the shader itself here (`envStars()`, from a binned copy of the catalogue), since the main view's stars are
point sprites no probe would see.

**SKY_REFL** additionally reads the mesh reflection G-buffer and discards every pixel that is not a
mirror-smooth pixel of the current instance; its view direction is the reflected ray stored there. See
[Satellite meshes](satellite-meshes.md#sharp-mirror-reflections).

## Settings

| UI label (tab) | `settings.json` key | Default |
|---|---|---|
| Rayleigh gain (Atmosphere) | `clouds.atmos_rayleigh_gain` | 0.98 |
| Mie/haze gain (Atmosphere) | `clouds.atmos_mie_gain` | 1.0 |
| View samples (min) (Atmosphere) | `clouds.view_samples_min` | 6.5 |
| View samples (max) (Atmosphere) | `clouds.view_samples_max` | 160 |
| Light samples (Atmosphere) | `clouds.light_samples` | 2.4 |
| Zodiacal gain (Atmosphere) | `clouds.zodiacal_gain` | 0.01 |
| Zodiacal width (deg) (Atmosphere) | `clouds.zodiacal_width_deg` | 10 |
| Orbit colour grade (Atmosphere) | `clouds.orbit_grade` | 1.0 |
| Exposure (EV) (Photometry) | `clouds_v2.exposure_ev` | 0 |
| Tonemap (0 exponential / 1 AgX) (Photometry) | `clouds_v2.tonemap` | 1 |
| AgX saturation (Photometry) | `clouds_v2.tonemap_saturation` | 1.15 |
| AgX contrast (Photometry) | `clouds_v2.tonemap_contrast` | 1.15 |
| Highlight roll-off (Photometry; the exponential curve only) | `clouds_v2.highlight_rolloff` | 1.0 |
| Auto exposure (day) (Photometry) | `clouds_v2.auto_exposure` | 1.0 |
| White balance (Photometry) | `clouds_v2.white_balance` | 0.6 |
| Extinction (Photometry) | `photometry.extinction_coeff` | 0.18 |
| Sky model (0 single / 1 multi-scatter + ozone) (Clouds) | `clouds_v2.sky_multiple_scattering` | 1 |
| Ground albedo (sky light) (Clouds) | `clouds_v2.sky_ground_albedo` | 0.25 |
| God rays (Weather) | `clouds_v2.godrays` | 1.0 (0-1) |
| Terrain god rays (0/1) (Weather) | `clouds_v2.terrain_godrays` | 1 |
| Haze near clouds (Weather) | `clouds_v2.haze_aloft` | 1.9 |
| God-ray froxels (0/1) (Weather, Rebuild experiments) | `clouds_v2.godray_froxels` | 1 |
| God ray reads (near, 8 = full) (Weather, Rebuild experiments) | `clouds_v2.godray_reads` | 8 (2-8; 4 saves 0.5-1.1 ms of the sky pass) |
| Aerial perspective volume (0/1) (Weather, Rebuild experiments) | `clouds_v2.sky_ap` | 1 |
| Aerial perspective horizon band (deg) (Weather, Rebuild experiments) | `clouds_v2.sky_ap_band_deg` | 2 |
| (no UI) the loop's step multiplier inside the band | `clouds_v2.sky_ap_band_steps` | 1 |
| Temporal anti-aliasing (Display) | `display.sky_taa` | on |
| (no UI) | `display.sky_taa_weight` / `display.sky_taa_weight_moving` | 0.1 / 0.35 |
| Render scale (Display) | `display.render_scale` | 1.0 (the preset sets it) |
| Clouds follow render scale (Display) | `display.clouds_follow_render_scale` | off (the lighter presets turn it on) |
| Storm sea trails (Ocean) | `clouds.ocean_storm_trails` | 1.0 |

The graphics presets override the sample counts and render scale; see
[Graphics settings](../using/graphics-settings.md). The automatic render scale's settings are listed on
[Weak-hardware tiers](hardware-tiers.md#settings).

## Where in the code

- `shaders/sat_sky.frag`: `main()`, `skyAgX()`, `evalCloudLayer()`, `lensFlare()`, `solarCoronaAt()`.
- `shaders/include/sky_atmos.glsl`: `atmHalfAvg3()`, `optDepth()`, `sunPathTauExact()`, `earthShadowSpan()`,
  `earthShadowLit()`, `skyZenithSky()` (shared by the sky pass and `sky_ap.comp`).
- `shaders/sky_ap.comp`, `shaders/include/sky_ap.glsl`: the aerial-perspective volume (`skyApSlice()`,
  `skyApNorm()`, `skyApRead()`).
- `shaders/include/cloud_lightning.glsl`: `cv2FroxJitter()`, `cv2GodFroxT()`, `cv2StvTFine()`, `cv2TerrSunTFine()`
  (the god rays), `cv2SkyPsiAt()` (multiple scattering); `shaders/cloud_v2_froxel.comp` fills the froxels and
  `shaders/cloud_v2_sunlut.comp` bakes \(\Psi\).
- `shaders/sky_taa.comp`, `shaders/taa_depth_restore.frag`, `shaders/taa_fullscreen.vert`.
- `shaders/include/darksky.glsl`, `shaders/include/atmosphere.glsl` (`atmColumn()`, `atmOzoneTau()`),
  `shaders/include/common.glsl`.
- `src/simulations/SatelliteSimCloudsV2.cpp`: the froxel and aerial-perspective dispatches in `recordCloudsV2()`,
  `ensureSkyAp()`.
- `src/simulations/SatelliteSim.cpp`: `buildSkyDrawPC()`, the `CloudParams` fill in `recordCompute()`,
  `recordPrePass()`, `recordSkyTaa()`, `skyTaaWanted()`, `skyTaaInExtent()`, `computeHalfExtent()`,
  `recreateComputeScaledTargets()`, `updateDynamicResolution()`, `readExposureMeter()`, `skyExposure()`,
  `recordScreenshotCopy()`.
