# Terrain / Earth Rendering Plan

Tracks implementation of Earth texturing, terrain elevation, ocean materials, night lights,
clouds, and orbital camera mode. Read this at the start of any terrain-related session.

---

## Architecture Summary (from design session 2026-06-22)

### Rendering approach
- Fullscreen triangle + fragment shader (existing). Ground intersection replaces the current
  flat `smoothstep` ground blend with proper sphere ray-casting.
- Raymarched terrain: two-phase march (coarse atmosphere → fine height-field), elevation map
  as a displacement above R_EARTH. Forward-compatible with orbital camera mode.
- All Earth textures sampled via equirectangular UV derived from ECI hit point + GMST.

### Coordinate notes
- GMST is needed in `sat_sky.frag` to convert ECI hit points to geographic lat/lon UV.
  Added to `SatDrawPC` at offset 72 (repurposed `pad[0]`). Filled in `recordDraw()`.
- Orbital camera mode is a later, larger rework: promote `obsPos` to push constant,
  generalize horizon tests from `dir.z < 0` to sphere intersection, replace elevation-based
  sat cull with frustum dot product.

### Descriptor set plan (sky pipeline, bindings 0-9)
| Binding | Current | Future |
|---------|---------|--------|
| 0 | GlowBuf SSBO | GlowBuf SSBO |
| 1 | noiseTex sampler | noiseTex sampler |
| 2 | moonTex sampler | moonTex sampler |
| 3 | earthDay sampler (8K SRGB) | earthDay sampler |
| 4 | earthNight sampler (8K SRGB) | earthNight sampler |
| 5 | earthElev sampler (R8_UNORM) | earthElev sampler |
| 6 | earthSpec sampler (R8_UNORM) | earthSpec sampler |
| 7 | — | earthClouds sampler (8K R8) — Step 9 C1 |
| 8 | — | cloudNoise sampler3D (128³ RGBA Perlin-Worley/Worley) — Step 9 C6 |
| 9 | — | CloudParams UBO (cloud/fog/beam tunables) — Step 9 C2 |

Note: bindings 7-8 were reassigned from an earlier speculative reservation (was earthNormal /
earthClouds). The earth normal map (`8k_earth_normal_map.png`) exists in assets but is **not** part
of the clouds work and has no active binding reservation; if added later it takes the next free
binding. A `ReflectBeams` SSBO (Step 9 C12) also takes the next free binding when implemented.

Adding bindings requires rebuilding `skyDescLayout`, `skyDescPool`, and `skyDescSet`.
See `createDescriptors()` area ~line 2418 in `SatelliteSim.cpp`.

### Mip-chain requirement
`ctx.createImage()` currently creates single-mip images. 8K textures at grazing angles
alias badly without full mip chains. Need a `createImageWithMips()` variant that:
1. Creates image with `mipLevels = floor(log2(max(w,h))) + 1`
2. Uploads base level via staging buffer
3. Generates remaining levels with `vkCmdBlitImage` in a one-time command buffer
4. Transitions all mips to `SHADER_READ_ONLY_OPTIMAL`

---

## Step Checklist

### Infrastructure (blockers for everything)
- [x] **Step 1** — Convert TIF textures to PNG  
  `magick 8k_earth_normal_map.tif → .png` and `8k_earth_specular_map.tif → .png`  
  Done session 2026-06-22. PNGs land in `assets/textures/`.

- [x] **Step 3** — GMST push constant in `SatDrawPC`  
  `pad[0]` → `gmst` (offset 72, float). Filled in `recordDraw()` from `simDayJ2000`/`simSecInDay`.  
  Declared in `sat_sky.frag` and `sat_sky.vert` PC blocks. No functional use yet.  
  Done session 2026-06-22.

- [x] **Step 2** — Confirm / add elevation map asset  
  `earth_elevation.png` (converted from .jpg session 7): 21600×10800 grayscale 8-bit land-only DEM.  
  **NOT ETOPO1. NO bathymetry. pixel=0 is NOT sea level.**  
  Actual encoding: pixel=15/255 → sea level (0 m); pixel=255/255 → ~8848 m (Everest).  
  Ocean region stores ~15/255 as baseline. Shader corrects with `kElevOffset = 15/255 × kElevRange ≈ 529 m`.  
  Formula: `terrainH = max(0, pixel × kElevRange − kElevOffset)`.  
  Omitting the offset creates a ~530 m cliff at every coastline worldwide.

- [x] **Step 4** — Mip-chain support in `createImage()` / add `generateMipmaps()`  
  `VulkanContext::createImage` now accepts optional `mipLevels = 1` param.  
  New `generateMipmaps(cmd, img, fmt, w, h, mipLevels)` does the per-mip blit chain.  
  Done session 2026-06-22.

### Earth sphere texturing (first big visual result)
- [x] **Step 5** — Load Earth day/night textures (bindings 3-4)  
  `SatDrawPC` expanded to 128 bytes: added `obsECEFDir (vec4)` at offset 112.  
  Filled from `obsDir` (ECEF unit vec) each frame in `recordDraw()`.  
  Added `earthDayImg/Mem/View/Sampler` and `earthNightImg/…` members.  
  Both textures load as `VK_FORMAT_R8G8B8A8_SRGB` with full mip chains.  
  Sky descriptor set expanded from 3 to 5 bindings (day=3, night=4).  
  Done session 2026-06-22.

- [x] **Step 5b** — Sphere intersection + texture sampling in `sat_sky.frag`  
  Ground branch now: `raySphere(obsPos, dir, R_EARTH)` → hit point →  
  ENU→ECEF via `pc.obsECEFDir` → geographic lat/lon → equirect UV →  
  sample earthDayTex/earthNightTex; Lambertian blend by `dot(hit, sunDir)`.  
  Night city lights at 12% intensity; fallback to flat colour if no sphere hit.  
  Done session 2026-06-22.

- [x] **Step 6** — Ocean wave material (redesigned session 5)  
  `earthSpecTex` loaded as R8_UNORM with mips (binding 6); sky desc set 6→7 bindings.  
  In Phase 3 (sea-level hits only, tHit < 0): sample specular mask → for ocean pixels:  
  - `pc.pad` → `pc.waveTime` (wall-clock seconds, constant speed regardless of time warp)  
  - 4-octave wave normals in ENU metric space (hitPt.xy metres, not lat/lon):  
    50 m / 180 m / 650 m / 2300 m; exponential distance fade per octave  
    Octaves beyond 12 km flatten via smoothstep → only glint/Fresnel visible from orbit  
  - Sky reflection: 6-sample atmosphere loop in reflect(dir, waveN) direction  
    Fresnel-mixes reflected sky color (or dark Earth absorption if reflDir.z < 0)  
    Fades to approximate color beyond 30 km  
  - Deep-water albedo 5%; Blinn-Phong glint (exp=300) always active for orbit glitter  
  Depth-occlusion for satellites/stars behind terrain also added this session:  
  - `gl_FragDepth` written in `sat_sky.frag` (terrain < 150 km → [0,0.5); sky → 1.0)  
  - Sky pipeline: depthWrite=true, ALWAYS; sat+star pipelines: depthTest=true, LESS.

- [ ] **Step 7** — Night lights → sky glow (light pollution simulation)  
  **Blocked on Step 9 (volumetric atmosphere).** See design notes below.

  ### Why horizon-only sampling doesn't work

  An attempted implementation shot 8 rays at the geometric horizon, sampled
  `earthNightTex` at each surface hit, and spread a warm Gaussian over nearby sky
  directions. This produced three problems:

  1. **Wrong geometry**: when the observer is inside a city, glow should be brightest at
     the zenith (light scattering straight up through the local atmosphere column), not
     clamped to the horizon. The horizon-only approach inverts this entirely.

  2. **No distance-correct fall-off**: glow from a city 2,000 km away should be far dimmer
     than glow from local streets. 8-point horizon sampling cannot encode distance.

  3. **No physically motivated spread**: how wide the glow dome extends above a city
     depends on how far light travels through the lower atmosphere before scattering out.
     That is exactly what the N_VIEW atmosphere ray-march already computes — but for
     sunlight, not city upwelling.

  ### Correct implementation (requires volumetric atmosphere — do alongside Step 9)

  City light pollution is an **upwelling radiance source** distributed along the view
  ray's atmosphere segment. The right place to add it is inside the existing `N_VIEW`
  march loop, at each sample point `sp`:

  ```glsl
  // Inside the N_VIEW atmosphere loop, after computing sp, densR, and attn:
  vec3  spECEF = sp.x * enuX + sp.y * enuY + sp.z * enuZ;
  float spLat  = asin(clamp(spECEF.z / length(spECEF), -1.0, 1.0));
  float spLon  = atan(spECEF.y, spECEF.x);
  vec2  spUV   = vec2((spLon + PI) / (2.0*PI), (0.5*PI - spLat) / PI);

  // textureLod mip 4 ≈ 1350×675 texels — sufficient for this low-frequency signal.
  float spLum  = dot(textureLod(earthNightTex, spUV, 4.0).rgb,
                     vec3(0.2126, 0.7152, 0.0722));
  float cityUp = max(0.0, spLum - kNightFloor);  // strip dim-blue airglow baseline

  // densR already weights low atmosphere heavily (exp(-h/H_R)).
  // attn is the view-ray transmittance already accumulated to this step.
  accumCity += cityUp * densR * attn;
  ```

  After the loop: `color += accumCity * vec3(1.0, 0.72, 0.42) * nightFactor * kNightGlowScale`

  **Why this is correct:**
  - Observer over a city → every low-altitude sample has high `cityUp` → strong zenith glow
  - Observer 2,000 km away → only the lowest-elevation horizon samples light up → dim horizon glow
  - Altitude fall-off is natural: `densR = exp(-h / H_R)` weights near-surface atmosphere
  - `attn` handles self-shadowing along the view ray automatically

  **Cost**: 1 `textureLod` call per N_VIEW step (currently 124 steps × all sky fragments).
  The `nightFactor` early-out skips the work entirely in daylight.

  ### City signal extraction
  The 8K night texture tonal bands:
  | Luminance | Content |
  |-----------|---------|
  | 0–0.05 | Dark ocean / uninhabited land — airglow + instrument noise |
  | 0.05–0.10 | Sparse settlements, lit road corridors |
  | > 0.10 | Cities and industrial areas — primary light pollution signal |

  `cityLum = max(0, lum - kNightFloor)` with `kNightFloor ≈ 0.06` strips the background.
  Use `textureLod(..., 4.0)` (not `texture`) to avoid aliasing at 124 steps.

### Terrain elevation
- [x] **Step 8** — Elevation raymarching  
  Elevation texture loaded as `VK_FORMAT_R8_UNORM` with full mip chain (binding 5).  
  Two-phase terrain march in `sat_sky.frag`:  
  - Phase 1: 48 uniform steps, march range [0, min(tBase.x, 300km)]  
  - Phase 2: 8-step binary search on first terrain overshoot  
  - Terrain hit shaded same as sea-level (sphere-normal Lambertian + day/night textures)  
  - Sea-level fallback if no terrain hit; flat colour if no sphere intersection.  
  No terrain-slope normals yet (use sphere normal for shading). Done session 2026-06-23.

### Clouds + Volumetrics (Step 9 — staged; folds in Step 7)

Full design: clouds + a reusable participating-media framework (clouds, fog/dust, volumetric city-
light pollution, Reflect-Orbital sky beams). Must be **seamless across the full camera range** (ground
via WASD → orbit via QE elevation) — the **ocean `altFade` LOD is the reference**: full detail close,
simplified overlay from far/high, cross-faded. Decisions locked: **3D noise textures from the start**
(true Nubis), **seamless at all altitudes**, **full suite staged** as small independent sessions.

#### Methodology: Nubis Cubed, adapted to a planetary sim
| Nubis concept | This project |
|---|---|
| 2D weather map (coverage/height/type) | Existing `8k_earth_clouds.jpg` (grayscale coverage), Earth-fixed, slow drift |
| Low-freq 3D Perlin-Worley base | Baked 128³ RGBA volume (R = Perlin-Worley, GBA = Worley octaves) |
| High-freq 3D Worley detail (erosion) | GBA channels now; optional separate 32³ detail later |
| Vertical height profile (stratus/cumulus/cumulonimbus) | Analytic height-gradient functions over the shell |
| `density = remap(base, 1-coverage, 1)×heightGrad − detail·erosion` | Same in `cloudDensity()` |
| Beer × Powder × HG, cone light-march, multi-scatter octaves, ambient | Beer-Powder + dual-lobe HG + ~6-sample sun cone (reuse `optDepth`) + N-octave multi-scatter + sky ambient |
| Adaptive march + empty-space skipping | Bounded shell march, big steps until density, small steps inside |

The jpg is the **coverage signal**; the 3D noise supplies **shape/detail**; height profiles supply
**vertical structure**. No authored "hero" voxel clouds needed at planetary scale.

#### Architecture (two integrators, one philosophy)
1. **Cheap per-step effects ride the existing atmosphere loop** (`sat_sky.frag:491-514`, N_VIEW=124):
   city upwelling (Step 7) + optional thin haze = one extra texture read per step. No new march.
2. **A dedicated bounded cloud-shell march** for volumetric clouds (and where beam in-scatter is
   sampled): marches only the segment inside the cloud shell, adaptive steps + short sun cone-march.
- **LOD (ocean pattern):** `cloudAltFade` cross-fades the cheap 2D overlay (dominant far/high) with
  the volumetric march (dominant within a few shell-thicknesses). Mirrors
  `altFade = 1 - smoothstep(3000, 8000, obsEffH)`. `obsEffH`/`obsPos` already in the shader.
- **Reuse:** `raySphere()` (`:86`) shell hits · `optDepth()` (`:94`) sun cone-march template ·
  `phaseR/phaseM` (`:78-85`) HG · terrain `tHit`/`tSeaLvl`/`tSurface` (`:~475`) = **the "distance map"
  for godrays** (march far-bound + beam occluder) · ocean `altFade` (`:~676`) cross-fade template ·
  `glowBuf` SSBO · `ctx.createImage`/`generateMipmaps`/`createBuffer` · settings + `PhotoParam` sliders.
- **Tunables in a `CloudParams` UBO** (binding 9), not push constants — many knobs, and it plugs into
  `settings.json` + the settings-window sliders and avoids growing `SatDrawPC`.

#### Phase A — Cloud-map overlay (cheap, ships first)
- [x] **C1 — Load cloud map (binding 7).** `assets/textures/8k_earth_clouds.jpg` as R8_UNORM + mips
  (clone earthSpec load, `SatelliteSim.cpp:~2772-2844`); add `earthClouds{Img,Mem,View,Sampler}` +
  cleanup; expand sky desc set 7→8 (layout/pool/writes, `:~2846-2933`); declare `sampler2D
  earthCloudsTex` binding 7 in `sat_sky.frag`. *Done when:* build + validation clean, texture bound.
- [x] **C2 — `CloudParams` UBO (binding 9) + settings + UI.** Struct (H + GLSL UBO), host-visible
  mapped, updated each frame. Fields: `coverage, density, baseAltM, topAltM, driftRate, sunGain,
  ambientGain, hgG, marchSteps, lightSteps, cloudPhase` (continuous `fmod(driftRate*t, 2π)`) + reserved
  fog/beam fields. Persist (`loadSettings`/`saveSettings` `:~3723`/`:~3830`) + sliders (`PhotoParam`
  `:~1577`). *Done when:* sliders live-update and persist.
- [x] **C3 — 2D overlay on the planet (refactored to shell layer in session 14).** Sample `earthCloudsTex` at the surface hit's lat/lon (same
  Earth-fixed UV path as day/night) with a `cloudPhase` longitude offset so clouds rotate **slightly
  faster than the ground**. Sun-lit (day) + dim ambient (night); composite over surface; overlay
  dominant when far/high. *Done when:* clouds visible from altitude, drifting faster than surface.
- [x] **C4 — High cirrus 2D layer.** Thin semi-transparent layer on a `raySphere` shell ~10-12 km;
  scroll faster than C3; composite along view ray. *Done when:* high veil visible, thins from orbit.

#### Phase B — 3D noise infrastructure (chosen up front)
- [ ] **C5 — 3D image support in `VulkanContext`.** Add `depth` param (or `createImage3D`) →
  `VK_IMAGE_TYPE_3D` + `VK_IMAGE_VIEW_TYPE_3D`; allow `VK_IMAGE_USAGE_STORAGE_BIT`
  (`VulkanContext.cpp:635`, header `:77-80`). *Done when:* a 3D image can be created/written/sampled.
- [ ] **C6 — Bake the noise volume.** New `shaders/cloud_noise.comp` writes a 128³ RGBA volume
  (R = Perlin-Worley, GBA = Worley octaves) into a storage 3D image once at init; barrier
  GENERAL→SHADER_READ_ONLY; bind `sampler3D` binding 8; dispatch from `init()`. *Done when:* debug
  view shows tiling 3D noise.

#### Phase C — Volumetric cloud march (Nubis Cubed)
- [ ] **C7 — Shell-march scaffold (extinction only).** `cloudShell(ro,rd)` via `raySphere`(base/top)
  clipped to `[0, tSurface]` (terrain distance = far bound + occluder); handle observer below/inside/
  above the shell (seamless all-altitude). `cloudDensity(p)` = coverage × heightProfile × detail(3D)
  with remap + erosion. Adaptive stepping + empty-space skip; gray extinction only; cross-fade with
  C3 overlay via `altFade`. *Done when:* shapes track the map, no march artifacts, seamless ground↔orbit.
- [ ] **C8 — Cloud lighting.** Beer-Powder transmittance + dual-lobe HG + sun cone light-march (~6
  samples, reuse `optDepth` structure) + N-octave multi-scatter + sky ambient; front-to-back in-scatter/
  transmittance accumulation. *Done when:* lit tops, dark bases, silver lining, believable dawn/dusk/night.
- [ ] **C9 — Composite & performance.** Blend cloud `(T, inscatter)` with atmosphere + terrain in
  order; write `gl_FragDepth` so satellites/stars behind dense cloud are occluded (mirror terrain depth
  `sat_sky.frag:951-961`). Step counts in UBO; optional half-res buffer + upsample / temporal jitter.
  *Done when:* clouds correct at all times of day, occlude satellites, hold framerate.

#### Phase D — Other volumetrics (reuse the integrator)
- [ ] **C10 — City light-pollution upwelling (= Step 7).** Add the night-texture upwelling term in the
  atmosphere loop per the Step 7 spec above (`accumCity += cityUp*densR*attn`, mip-4 `earthNightTex`,
  strip `kNightFloor`, gate by `nightFactor`); modulate by cloud transmittance (cities glow into
  overcast). **Mark Step 7 done here.** *Done when:* glow domes correct, distance fall-off, brighter under cloud.
- [ ] **C11 — Fog / dust / haze.** Height-based exponential medium (0-~2 km) + tint + optional 3D-noise
  patchiness; analytic base extinction + sampled in-scatter near camera. Softens twilight + light
  pollution; the low medium beams scatter through. *Done when:* low haze reads near ground, recedes with altitude.
- [ ] **C12 — Reflect-Orbital sky beams.** New `ReflectBeams` SSBO (sat ENU pos + ground target +
  intensity), written by the compute pass (mirror target/normal already at `sat_orbit.comp:272-313`) or
  CPU, capped to N nearest active beams. In the cloud/fog march, accumulate beam in-scatter
  `phase(view·beam) × proximity(dist to beam line) × mediumDensity(p) × shadow(cloud/terrain)`.
  *Done when:* visible shafts from sunlit Reflect sats to night-side targets, occluded by cloud/terrain.

#### Notes for a smaller model picking this up
- Do C1→C12 **in order**; each is a self-contained session with a "Done when" gate. A/B ship before C
  exists. **Don't start C7 before C5/C6 (3D noise) are proven.**
- Respect the **elevation encoding** rule (above / in `CLAUDE.md`): the 529 m ocean-baseline offset
  bug has been reintroduced repeatedly — don't touch terrain height without it.
- The cloud overlay UV uses the **same Earth-fixed lat/lon path** as day/night; only add the
  `cloudPhase` longitude drift — do not invent a separate projection.
- Keep every tunable in the `CloudParams` UBO so it auto-flows into settings + the slider UI.

### Orbital camera mode (largest rework)
- [ ] **Step 10** — Orbital camera / coordinate decoupling  
  - Promote `obsPos` to push constant in `SatDrawPC` (expand struct or reuse existing fields)
  - Generalize sky shader: remove hardcoded `obsPos = (0,0,R_EARTH+1)`, use push constant
  - Replace `dir.z < 0` horizon tests with `raySphere(obsPos, dir, R_EARTH).x > 0`
  - In `sat_flare.comp`: replace elevation-based cull with frustum dot product
  - Add `CameraMode` enum; `updatePositions()` builds view matrix differently per mode
  - Movement code: WASD in world space when orbital, surface-tangent when ground

---

## Key File Locations

| File | Role |
|------|------|
| `src/simulations/SatelliteSim.h` | `SatDrawPC` struct (push constants), member declarations |
| `src/simulations/SatelliteSim.cpp` | `recordDraw()` (PC fill), `createDescriptors()` (sky bindings ~L2418) |
| `shaders/sat_sky.frag` | Main sky + ground rendering, all texture sampling goes here |
| `shaders/sat_sky.vert` | Fullscreen triangle, passes enuDir + sun/moon to frag |
| `shaders/sat_point.vert` | Satellite point sprite, shares SatDrawPC layout |
| `assets/textures/` | All Earth texture PNGs |

---

## Session Log

### 2026-06-22 (session 1)
- Architecture design discussion covering all 10 steps
- Confirmed ImageMagick available; kicked off TIF → PNG conversion
- Added GMST to `SatDrawPC` (repurposed pad[0]), filled in `recordDraw()`,
  updated `sat_sky.frag` / `sat_sky.vert` / `sat_point.vert` PC declarations
- Steps 1 and 3 complete; step 2 (elevation map) and step 4 (mip chains) pending

### 2026-06-22 (session 2)
- Step 4: `VulkanContext::createImage` gains optional `mipLevels = 1`; new `generateMipmaps()` helper
- Step 5: `SatDrawPC` expanded 112→128 bytes (`obsECEFDir vec4` at offset 112)
  Earth day + night textures loaded as SRGB with full mip chains; sky descriptor set 3→5 bindings
- Step 5b: `sat_sky.frag` sphere intersection + ENU→ECEF→lat/lon UV + day/night texture blend
  Replaces flat smoothstep ground colour with textured Earth surface
- Build clean; `earth_elevation.jpg` present (needs inspection for Step 2 / Step 8)

### 2026-06-23 (session 3)
- Step 2: Confirmed `earth_elevation.jpg` is 21600×10800 8-bit grayscale land-only DEM (NOT ETOPO1; see Step 2 above for correct encoding)
- Step 8: Elevation terrain raymarching implemented in `sat_sky.frag`
  - Elevation texture (R8_UNORM, full mip chain) added as binding 5; sky desc set 5→6 bindings
  - Ground section refactored: 48-step coarse march + 8-step binary search
  - March range: [0, min(tBase.x, 300km)]; tShell.y used when ray is upward (horizon mountains)
  - Terrain hits shade with sphere-normal Lambertian (same as sea-level path)
  - Graceful fallback: if elevation file missing, placeholder sampler used (march finds no hits)
  - Build clean

### 2026-06-24 (session 5)
- Step 6 redesigned: ocean wave material fully rewritten
  - `pc.pad` → `pc.waveTime` (wall-clock, constant speed regardless of time warp)
  - Wave UV basis: ENU `hitPt.xy` (metres) → physically correct scale at all altitudes
  - 4 octaves: 50 m, 180 m, 650 m, 2300 m; exponential distance fade
  - Sky reflection: 6-sample atmosphere loop in reflected direction; Fresnel mix
  - Deep-water albedo 5%; glint (exp=300) active from orbit
- Build clean; sat_sky.frag.spv regenerated

### 2026-06-25 (session 6)
- Step 6 wave upgrade: analytic Seascape FBM + Earth-fixed coordinates
  - Replaced 4-octave noise-texture approach with ShaderToy "Seascape" (Ms2SD1) analytic model
  - `seaOctave(uv, choppy)`: 1−|sin(uv)| shape + low-mip noiseTex perturbation to break tiling
  - `seaHeight(posM, t)`: 5-octave FBM, ~628 m → ~3 m wavelengths; kWaveRot×1.9 per octave
  - Wave UV basis: **ECEF-XY** (`hitECEF.xy`) instead of ENU `hitPt.xy`
    Waves now stationary relative to Earth surface — fix for pattern sliding with observer
  - Normal from central differences on seaHeight; wEps adapts with distance
  - ECEF gradient projected to ENU via `vec3(enuX.x, enuY.x, enuZ.x)` and `.y` equivalents
  - Reverted debug threshold `tHit < 5000.0` → `tHit < 0.0` (specMask JPEG fix is in place)
  - Build clean

### 2026-06-24 (session 4)
- Satellite occlusion from ground: `gl_FragDepth` in sky shader (terrain < 150 km → [0, 0.5); sky → 1.0)
  Sky pipeline depth write ALWAYS; sat/star pipelines depth test LESS. Satellites/stars now
  hidden behind mountains.
- Elevation encoding: kElevRange set to 9000. GPU-side observer ground height lookup added.
  NOTE: the "pixel=0 → 0 m" assumption recorded here was WRONG — see session 7 correction below.
- Terrain march improvements: quadratic step distribution (96 steps), 12-step binary search,
  adaptive marchCap (250 km ground, up to 3000 km from orbit), jitter in normalised fraction.
- Sun/Moon Earth-limb sharp cutoff (8° fade → 0.008 sinEl window), antimeridian seam fix.
- Step 6: Ocean wave material complete (see checklist above).

### 2026-06-25 (session 7)
- **Elevation encoding bug fixed (global coastline cliff):**
  Every shoreline on Earth had a ~530 m vertical cliff. Root cause: the DEM stores its
  ocean/sea-level baseline as pixel=15/255, NOT pixel=0. The shader was computing
  `terrainH = pixel × 9000`, giving 529 m for sea-level land.
  Fix: added `kElevOffset = 15.0/255.0 × kElevRange` (≈ 529 m) and subtracted it from
  all 6 elevation reads in `sat_sky.frag` plus the CPU observer-height formula in
  `SatelliteSim.cpp`. Threshold in CPU code corrected from `< 5/255` to `<= 15/255`.
  **The DEM is NOT ETOPO1 and has NO bathymetry. pixel=0 ≠ sea level. pixel=15/255 = sea level.**
- `earth_elevation.jpg` → `earth_elevation.png` (lossless; loader updated in SatelliteSim.cpp)

### 2026-06-27 (session 8)
- Step 7 (night lights / light pollution) designed and attempted, then reverted:
  - Horizon-only approach (8-azimuth raySphere + Gaussian spread) was implemented and built
    clean, but produces wrong spatial distribution — glow clamps to the horizon rather than
    filling the zenith when the observer is inside a city
  - Root cause: light pollution requires in-atmosphere upwelling integration along the view ray,
    not a post-hoc angular spread from horizon samples
  - Correct implementation belongs inside the N_VIEW atmosphere loop alongside Step 9 (clouds)
  - Step 7 deferred; design spec and correct GLSL pseudocode recorded above in Step 7 entry
  - `sat_sky.frag` reverted to pre-Step-7 state; build clean

### 2026-06-27 (session 11)
- **C2 complete:** `GpuCloudParams` struct (48 bytes, std140) added to header; 10 CPU-side cloud float members + `cloudParamsBuf/Mem/Mapped`; sky desc set expanded to 9 bindings (0-7 image/SSBO + binding 9 UNIFORM_BUFFER, skipping 8 for C6 cloudNoise); `layout(binding=9) uniform CloudParams` declared in `sat_sky.frag`; `recordDraw` uploads UBO each frame including computed `cloudPhase`; 10-slider "Clouds" settings section (coverage, density, base/top alt, drift rate, sun/ambient gain, HG g, march/light steps) with full persist in `settings.json`. Build clean.

### 2026-06-28 (session 14)
- **C3/C4 refactored into generalized layer system:**
  - `GpuCloudLayerParams` struct (32 bytes, std140) added to header: `shellAltM, driftMult, alphaMax, mipLod, coverageMult, densityMult, enabled, pad`
  - `GpuCloudParams` restructured: removed `baseAltM`/`topAltM` from global section; added `layers[4]` (176 bytes total); `cloudBaseAltM`→layer0 shell alt (default 2000m), `cloudTopAltM`→layer1 shell alt (default 11000m)
  - `evalCloudLayer()` GLSL function added: takes ray + layer params, intersects shell, converts hit to ECEF, samples texture at drifted lat/lon UV, shades using `dot(cloudECEF, sunDirECEF)` — correct day/night at cloud's geographic position regardless of observer location
  - `sunDirECEF = sunDir.x*enuX + sunDir.y*enuY + sunDir.z*enuZ` computed once in main()
  - C3 surface-paste block removed; C4 inline block removed
  - Replaced by a `for (li in 0..3)` loop calling `evalCloudLayer()` per enabled layer
  - Layer 0: low cloud shell at ~2km, alphaMax=0.80, mipLod=0.0, coverageMult=1.0
  - Layer 1: cirrus shell at ~11km, alphaMax=0.15, mipLod=2.0, coverageMult=0.5, densityMult=0.4
  - Slider labels: "L0 alt (m)" / "L1 alt (m)" with updated ranges

### 2026-06-28 (session 13)
- **C4 complete:** High cirrus 2D layer in `sat_sky.frag` (shader-only).
  After the `if (tSurface > 0.0)` block, before auto-exposure:
  - `raySphere(obsPos, dir, R_EARTH + 11000)` — picks first positive hit (x > 0 from orbit, else y from ground)
  - Gated: `tCirrus < tSurface` so downward ground-observer rays don't hit cirrus below them
  - `cirrusUV.x = fract(lonUV + cloudPhase * 2.0 / 2π)` — drifts 2× faster than C3 surface clouds
  - `textureLod(earthCloudsTex, cirrusUV, 2.0)` — mip 2 for softer filamentous appearance
  - `cirrusAlpha = clamp((raw − (1−coverage)) × density × 0.4, 0, 0.15)` — max 15% opacity
  - Night-side: `cirrusDayFrac = smoothstep(-0.1, 0.15, sunDirENU.w)` → `cirrusColor = vec3(0)` at night; no fixed ambient
  - `color = mix(color, cirrusColor * cirrusAttn, cirrusAlpha)` — composited over atmosphere + surface
  - C3 night-side brightness noted as a bug (ambientGain 0.02 is still too bright vs ~0.001 night sky); deferred to C7 volumetrics which will replace C3/C4 shading entirely.

### 2026-06-27 (session 12)
- **C3 complete:** 2D cloud overlay in `sat_sky.frag` (shader-only change; all Vulkan wiring from C1/C2).
  After the ocean wave block, before `color += surfColor * surfAttn`:
  - `cloudUV.x = fract(uvSurf.x + cloud.cloudPhase / (2π))` — longitude drift from C2 UBO
  - `cloudRaw = textureGrad(earthCloudsTex, cloudUV, uvd_dx, uvd_dy).r` — reuses antimeridian-safe gradient
  - `cloudAlpha = clamp((cloudRaw − (1 − coverage)) × density, 0, 1)` — coverage threshold + density sharpness
  - `cloudColor = mix(ambientGain, max(0,sunDot) × sunGain, dayFrac)` — same sunDot/dayFrac as surface
  - `surfColor = mix(surfColor, cloudColor, cloudAlpha)` — composite over both terrain and ocean
  Build clean. Clouds visible from altitude with longitude drift controlled by drift rate slider.

### 2026-06-27 (session 10)
- **C1 complete:** `8k_earth_clouds.jpg` loaded as R8_UNORM with full mip chain; `earthClouds{Img,Mem,View,Sampler}` added to header + cleanup; sky desc set expanded 7→8 bindings (layout/pool/writes); `earthCloudsTex` declared at binding 7 in `sat_sky.frag`. Debug tint confirmed cloud bands on terrain, reverted. Build clean.

### 2026-06-27 (session 9)
- **Step 9 expanded into a full Clouds & Volumetrics plan** (replaces the old one-line stub).
  Design discussion locked: 3D noise textures from the start (true Nubis Cubed), seamless at all
  altitudes (ocean `altFade` LOD as reference), full suite staged (clouds → city upwelling → fog/dust
  → Reflect beams) sharing one participating-media integrator.
- Methodology mapped to Nubis Cubed (2D jpg = coverage, baked 128³ Perlin-Worley/Worley 3D volume =
  shape/detail, analytic height profiles = vertical structure; Beer-Powder + dual-lobe HG + sun cone
  light-march + multi-scatter octaves + ambient).
- Architecture: cheap effects (city upwelling, thin haze) ride the existing N_VIEW atmosphere loop;
  a dedicated bounded cloud-shell march does the volumetric clouds + beam in-scatter. Reuses
  `raySphere`/`optDepth`/phase funcs/terrain `tSurface` (distance map)/`glowBuf`/ocean `altFade`.
- Sky descriptor set grows 0-6 → 0-9: +earthClouds (7), +cloudNoise sampler3D (8), +CloudParams UBO (9).
  Binding table above updated (7-8 reassigned from old earthNormal/earthClouds reservation).
- 12 sub-steps C1-C12 across Phases A-D, each a self-contained session with a "Done when" gate,
  authored for smaller models in smaller sessions. No code changes this session — planning only.
