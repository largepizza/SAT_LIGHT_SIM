# The sky pass: atmosphere and composite

`shaders/sat_sky.frag` draws one fullscreen triangle and produces the background of the frame: atmosphere,
terrain, sea, meshes, clouds, Sun, Moon and Milky Way, tonemapped. It also writes the unified depth that
point sources are tested against. Its variants are listed in README §6.

The order of operations in `main()` is the order of this document.

---

## 0. Inputs

- **Push constant** `SatDrawPC` (128 B): the view, FOV, aspect, GMST, wave time, Sun and Moon ENU
  directions, and the observer's ECEF direction plus height offset.
- **CloudParams UBO** (binding 9): everything else that is per-frame uniform.
- **Images and buffers from earlier passes:**

| Binding | From | Holds |
|---|---|---|
| 10 / 11 | `cloud_march.comp` | `cloudTargetA` (radiance, signed-km occlusion) / `cloudTargetB` (transmittance, ground shadow), ½ res |
| 19 | `scene_depth.comp` | `sceneDepthTex`, ½ res, metres |
| 22 / 23 | mesh scene pass | mesh radiance / distance, full res (`imageLoad`) |
| 26 | `scene_depth.comp` | `terrainFrame.x` = the eye's ground height with detail |
| 29 | `cloud_v2_far.comp` | far cloud layer, full res |
| 0 | `sat_flare.comp` | 64-bin satellite sky-glow histogram |
| 20 | `sat_flare.comp` | ocean-glint list |
| 21 | CPU | Reflect-beam ground spots, solar parks |

## 1. The ray and the first surface

1. **View direction.** The interpolated, unnormalised ENU direction from the vertex shader. Under
   `SKY_TAA` it is offset by `taaJitter` pixels through its screen derivatives.
2. **Eye.** `obsEffH = terrainFrame.x`, the same detailed ground every pass uses. The eye is 2 m above it.
3. **Terrain.** The full-resolution terrain march, seeded from `sceneDepthTex`, then the surface
   classification ([TERRAIN_AND_SURFACES.md](TERRAIN_AND_SURFACES.md) §4), giving `tSurface`.
4. **Mesh.** `tMesh` from the full-res mesh distance. If it is nearer than the terrain or sea,
   `tSurface = tMesh`. From here on the atmosphere, every disc gate and the depth stop at the mesh.

## 2. Reading the clouds early

The half-res cloud targets are read **before** the atmosphere loop, because the loop needs to know where
the cloud is.

- **Upsampling.** A bilinear tap at the pixel's UV. Where a surface edge crosses the half-res grid (log
  depth range > 0.1 across the 2×2 texels), the colour is re-averaged **joint-bilaterally**: each texel is
  weighted by `bilinear · exp(−6 |log d_texel − log tSurface|)`. This removes stair-steps where a ridge
  meets cloud.
- **Far layer.** With `farBlend > 0` the far layer goes behind the half-res composite:

  ```
  cloudA.rgb += farBlend · L_far · cloudB.rgb
  cloudB.rgb *= mix(1, T_far, farBlend)
  ```

- **Opaque cloud distance.** `tCloudOcclude = cloudA.a ≥ 0 ? cloudA.a·1000 : −1`. Used for the depth and
  the disc gates.
- **Air-split distance** `tAirFrontM` (where the air "in front of" the cloud ends):
  - the four gathered texels' |alpha| distances, weighted by bilinear share × opacity;
  - texels with no real distance are ignored;
  - if there is none, the nearest real cloud;
  - a far-layer share uses the far sphere's distance.

  `tCloudFrontM` is the nearest real cloud.

## 3. Atmosphere: single scattering with extras

**Range.** From the atmosphere **entry** (`tStart`) to `min(atmosphere exit, tSurface)`. From orbit nearly
every step would otherwise fall in vacuum.

**Steps.** `N_VIEW = clamp(ceil(length / (100 km / viewSamplesMin)), min, max)` uniform midpoint steps.

**Phases.** Rayleigh `0.75(1 + cos²)`, Mie Cornette-Shanks with g = 0.26. Scattering coefficients are the
base values × the "Rayleigh" / "Mie" gains. Scale heights are 8 km and 1.2 km.

**Per step:**
- Camera-side optical depth: `odR_cam`, `odM_cam`.
- **Sun lit** (the sample sees the Sun past the Earth):
  - `attn = exp(−(βR(odR_cam + odR_sun) + 1.1 βM(odM_cam + odM_sun)))`, with the Sun's column from
    `optDepth` (N_LIGHT steps);
  - accumulates Rayleigh and Mie in-scatter `accumR/M`.
  - × the **orbital terminator gate**, above 40 km of eye altitude only.
  - × the **cloud-hidden Sun** term: `sunCloudT`, applied only to air below 8-14 km, within ~50 km of the
    eye and within ~3 km of the eye's line to the Sun.
  - × the **eclipse**: the visible share of the Sun's disc from the sample, plus `2 × eclipseSkyLight`
    (the sky light inside the Moon's shadow).
- **In the Earth's shadow**: accumulates *shadowed air* `shR/shM` instead. Afterward it is lit as
  isotropic in-scatter of half the zenith sky's radiance at its mean point, faded like the clouds' dusk
  term. Single scattering alone leaves the air beyond the terminator black.
- **City glow**: the night map under the sample → `accumCity`, attenuated toward the eye.
- **Airglow**: green (96 km) and sodium (90 km) bands, gated by the sample's own geographic night. The red
  band and the aurora are marched at half resolution in `cloud_march.comp` instead.
- **Front copies**: the share of each step in front of `tAirFrontM` also goes to `accumRF/MF` (and
  `shRF/MF`, and `accumCityFront` up to the nearest cloud).

**After the loop:**

```
color    = pR·βR·accumR + pM·βM·accumM + shadowed-air term + city dome + beam proximity glow + airglow
airFront = pR·βR·accumRF + pM·βM·accumMF + shadowed-air front term
cityGlowFront = (city glow in front of the nearest cloud) · smoothstep(40 km, 150 km, tCloudFront)
```

`color` holds the air along the **whole** path to the surface, including the stretch in front of any
cloud.

## 4. Surfaces and pre-tonemap terms

- **Moon disc.**
  - Drawn at its true topocentric position and size × "Moon size". It eases back to its true size near the
    Sun, so solar eclipses are geometrically right.
  - Tidally locked (the texture frame faces the Earth's centre); phase from the Sun direction;
    limb-darkened; earthshine.
  - Lunar eclipse per surface point: the Earth's disc against the Sun's, umbral red light.
  - Hidden by any surface or opaque cloud.
- **Satellite sky glow.** The 64 bins (8 azimuth × 8 elevation) of `sat_flare.comp`'s brightest
  satellites, as wide Gaussians. Weighted by the smaller of the air along the pixel's ray and the air
  toward the bin's direction.
- **Surface.** Land or sea shading, `× exp(−τ_cam)`, added. A mesh pixel adds the mesh radiance × the same
  transmittance.
- **Reflect-beam ground spots**, as part of the surface.
- **Flat 2D cloud layers** (`evalCloudLayer`).
  - Shown in the main view only when the volumetric march is knocked out.
  - They are the clouds of SKY_ENV probes, reflections, SKY_LITE and Potato, read from the same evolving
    weather cube.

## 5. The cloud composite

1. **Mesh pixel.** If the cloud's distance (|alpha| km) is beyond the mesh, the cloud is skipped
   (`B = 1, A = 0`). The march clamps only to the half-res depth, which misses thin panels.
2. **The composite:**

```glsl
color = color * cloudB.rgb + cloudA.rgb + (cityGlowFront + airFront) * (1 - cloudB.rgb);
```

**Reading it.**
- `color·B` attenuates everything behind the cloud: the surface, and the air **including** the stretch in
  front of the cloud.
- Adding `airFront·(1 − B)` restores the front stretch, so that stretch ends up unattenuated, as it should.
- `cloudA.rgb` is the cloud's own light, already attenuated by the air in front (`attn·L` from the march),
  plus aurora, red airglow, lightning, rain drops and beam lines and shafts.

**Why split it this way.** The air in front of a cloud is the sky pass's own integral, so a cloud on the
horizon sits in exactly the same haze as the clear sky beside it. City glow in front of a cloud is kept out
of its attenuation for the same reason. Otherwise night clouds along the horizon read as a black band.

## 6. Exposure, tonemap and grade

**Exposure.**

```
dayness  = clamp((sin sunEl + 0.2)/1.2)
exposure = mix(10, 1.8, dayness^0.4) · exp2(user EV + auto EV)
```

**In order:**
1. **White balance.** Divide by the colour of sunlight at the observer, luminance-normalised. It fades in
   with the Sun's elevation, so a low Sun does not turn every cloud beige.
2. **Tonemap.** `x = exposure·color`, `color = mix(1 − e^(−x), 1 − 1/(1 + x + x²/2), highlightRolloff)`:
   the same toe, a longer shoulder.
3. **Orbit grade** (30-300 km eye altitude, sunlit Earth only, by the Sun at the surface point): toward
   `1.16·x^2.2` and 70% chroma. This is fitted to Artemis II photographs of the Earth from space.
4. **Night floor**: a tiny bluish lift.

**Display-space terms**, after the tonemap and × the global exposure:
- **Milky Way.**
  - Visibility is the product of:
    - a dark-sky model (`darksky.glsl`: the sky background magnitude from light pollution and twilight
      against the panorama texel's magnitude);
    - moonlight;
    - extinction along the ray;
    - the Sun's glare;
    - "not behind a surface or the Moon";
    - cloud transmittance³.
- **Zodiacal light and gegenschein.** An analytic shape under the same gates.
- **Moon glow and halo.**
- **Sun disc and glare** (not in probes).
  - Three stacked glare lobes bridge the disc to the corona.
  - Sun colour by elevation.
  - Near the Moon the disc takes its true radius and is hidden under the Moon's disc. Glare and corona
    follow the visible share. At totality the solar corona is drawn (dipole field-line fibres, streamers,
    chromosphere).
  - Everything is × `min(cloud transmittance, sunCloudT)` and hidden by any surface.
- **Lens flare** (Sun only), × the cloud transmittance at the Sun's screen position, `sunCloudT`, and the
  eclipse.

## 7. Depth output

```
t = terrain hit ?? sea / far-land hit ?? opaque cloud distance;   t = min(t, tMesh);   else Moon distance
gl_FragDepth = t ? log2(t/1 cm)/log2(1e9 m/1 cm) : 1.0
```

`SKY_TAA` writes the same value to an R32F colour copy; SKY_ENV writes none.

## 8. Sky TAA

Used at render scale 1 when enabled, and not on the Potato or Lite tiers.

1. **Render.** `sat_sky_taa.frag` renders offscreen with an 8-sample Halton (2,3) sub-pixel jitter, into
   RGBA16F colour plus R32F depth.
2. **Resolve** (`sky_taa.comp`):
   - **Current-frame statistics**: 3×3 YCoCg mean and standard deviation.
   - **Reproject** each pixel's unjittered ray. Sky (depth ≈ 1) by rotation alone; a surface at its depth
     plus the eye's motion, both from the CPU in double, in this frame's ENU.
   - **Disocclusion**: the expected history depth must fall inside the **range** of the 2×2 history depths
     (± 0.004). Testing against a single texel rejected every silhouette pixel every frame.
   - **History**: a 9-tap Catmull-Rom read.
   - **Variance clip**: mean ± 1.25 sd.
   - **Weight**: 0.1 still → 0.35 moving (0.5-4 px). A neighbourhood suddenly brighter than its history
     forces the new sample (flashes).
   - **History validity**: cleared on a FOV or aspect change, or an eye jump over 20 km.
3. **Blit** the resolved colour into the swapchain.
4. **Depth restore.** The main pass opens with `taa_depth_restore.frag`, which writes the jittered depth
   back with colour writes off. Stars, planets and satellites then draw unjittered and depth-tested over
   the anti-aliased background.

The half-res cloud composite is inside this image, so cloud edges are also averaged on screen.

## 9. Auto exposure

Auto exposure is a closed loop on the **displayed** frame.

**Measuring** (`recordScreenshotCopy`, every frame while it is on): the central 60% of the presented image
is blitted to 64×36 and read back next frame.

**`readExposureMeter`, near the ground:**
- steps toward a linear mean of 0.32;
- darkens harder when more than 4% of pixels clip;
- only darkens (to −3 EV), and only by day.

**From 100-1500 km of altitude** it blends to a **spot meter on the lit Earth**:
- the mean of pixels brighter than 0.04 (black space would never darken a small bright Earth), toward 0.47;
- down to −5 EV.

**Easing.** τ = 0.8 s down, 1.6 s up.

**Effect.** The result scales every exposure-dependent term next frame:
- the sky;
- point-source reference magnitudes;
- the Milky Way.
