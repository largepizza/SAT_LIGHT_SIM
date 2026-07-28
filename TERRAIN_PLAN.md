# Terrain / Earth Rendering Plan

Tracks implementation of Earth texturing, terrain elevation, ocean materials, night lights,
clouds, and orbital camera mode. Read this at the start of any terrain-related session.

---

## Immediate Next Step

**Moonlight tuning is settled (sessions 25-26):** `moonSuppression=4.0` (satellites),
`cloud.moonGain=0.015` (terrain+clouds), `kStarMoonMaxDim=0.9` (stars, user-tuned directly in the
IDE) all confirmed good in-app. Terrain night ambient was tried and **removed** (session 26) —
user found it should stick to 0, so the slider and shader term are both gone; don't re-add without
new direction from the user.

**Directional light-pollution dome — implemented (session 26), not yet seen in-app.** Replaced the
old uniform `pc.lightPollution` scalar with an 8-azimuth-sector dome (`lightDomeAz[]` /
`lightDomeBuf`) for both satellites and stars — see session 26 log and `CLAUDE.md`'s "Subsystem:
Light Pollution Dome" for the full design. Build clean; needs an in-app look, especially from a
city at low altitude where the directional effect should actually be visible (versus the old
scalar's uniform dimming).

`moonBright` (both satellite and star versions) is still uniform across the whole sky — unlike
light pollution, it wasn't made directional this session and there's no current plan to.

Satellite-reflected light (satellites as light sources onto terrain/clouds) and aurora ground-cast
light remain fully unscoped (raised, not designed) — pick up whenever the user wants them.

**Performance: superseded by session 29's real profiling data.** The session-24 transmittance-LUT
hypothesis (re-benchmark after the `N_VIEW`/`N_LIGHT` sliders, drop `Light samples`, see if that's
the win) turned out to be the wrong target once real GPU-timestamp + knockout-toggle data existed:
`optDepth`'s isolated cost was consistently near-zero. Terrain (a real step-count bug) and aurora
(never resolution-scaled, never had its noise baked) were the actual dominant costs — both fixed
this session. See session 29 log for the full profiling toolkit (in-app GPU timestamps, knockout
toggles, `perf_profiles/profile_log.jsonl`, `tools/perf_analysis/`) and why the main atmosphere
loop specifically is NOT a good candidate for the same half-res treatment (terrain-distance-coupled
endpoint + feeds satellite glow attenuation). A transmittance LUT is not ruled out forever, just no
longer the presumed next step without fresh data motivating it specifically.

**C16 — Aurora (geomagnetic curtain primitive) — feature-complete since session 28; heavily
re-architected for performance in session 29.** Visual design/tuning closed 2026-07-16 (session 28
follow-up #22, see that entry for the full tuning history: brightness/exposure, day/night gating,
fold axis calibration, cloud occlusion ordering, ambient lighting, extinction, light-
pollution/moonlight suppression, edge blending, per-column variation, shimmer evolution). Session
29 then moved the sky curtain march itself out of `sat_sky.frag` into `cloud_march.comp` (half
resolution) and baked most of its noise into a texture (`aurora_noise.comp`) — a ~40fps-swing cost
down to a minor one. `sat_sky.frag` still owns `auroraGlowAt` (terrain/ocean ambient) and the ocean
reflection's aurora sample; `cloud_march.comp` owns the sky curtain itself. See session 29 log
before touching aurora code — same "architecture changed, old assumptions may not hold" warning the
session 23 cloud move below already gives.

C14 (anvil) remains not started; it was deliberately deferred again in favor of C15 per the
2026-07-12 session, and can be picked up whenever — it has no dependency on C15/C16.

**Architecture note for any future cloud/C16 work (session 23, 2026-07-13):** `cloudMarch()` and
`cirrusMarch()` no longer live in `sat_sky.frag` — they moved to a new half-resolution compute
shader, `shaders/cloud_march.comp`, dispatched once per frame in `recordCompute()`. `sat_sky.frag`
now samples two precomputed `RGBA16_SFLOAT` targets (bindings 10/11) instead of marching per full-
res pixel. See the session log entry below for the full design (why two targets, the combined-
attenuation algebra, the accepted terrain-occlusion approximation) before touching cloud code —
the architecture changed enough that assumptions from C7-C15 sessions about "the cloud march runs
in the sky fragment shader" no longer hold.

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
- [x] **C5 — 3D image support in `VulkanContext`.** Add `depth` param (or `createImage3D`) →
  `VK_IMAGE_TYPE_3D` + `VK_IMAGE_VIEW_TYPE_3D`; allow `VK_IMAGE_USAGE_STORAGE_BIT`
  (`VulkanContext.cpp:635`, header `:77-80`). *Done when:* a 3D image can be created/written/sampled.
- [x] **C6 — Bake the noise volume.** New `shaders/cloud_noise.comp` writes a 128³ RGBA volume
  (R = Perlin-Worley, GBA = Worley octaves) into a storage 3D image once at init; barrier
  GENERAL→SHADER_READ_ONLY; bind `sampler3D` binding 8; dispatch from `init()`. *Done when:* debug
  view shows tiling 3D noise.

#### Phase C — Volumetric cloud march (Nubis Cubed)
- [x] **C7 — Shell-march scaffold (extinction only).** `cloudShell(ro,rd)` via `raySphere`(base/top)
  clipped to `[0, tSurface]` (terrain distance = far bound + occluder); handle observer below/inside/
  above the shell (seamless all-altitude). `cloudDensity(p)` = coverage × heightProfile × detail(3D)
  with remap + erosion. Adaptive stepping + empty-space skip; gray extinction only; cross-fade with
  C3 overlay via `altFade`. *Done when:* shapes track the map, no march artifacts, seamless ground↔orbit.

  **C7 tuning notes (2026-06-30):**
  - `cloudDensity` now takes two UVW coordinates: `uvwPresence` (Z=posZ, constant per column) for the
    Perlin-Worley R channel presence threshold, and `uvwDetail` (Z=hNorm×kVertTiles, original formula)
    for the Worley erosion G/B/A channels. This separates the cloud-existence decision (horizontal
    patchiness from XY noise) from the cloud-interior texture (3D Worley blobs from altitude variation).
    The Perlin R channel has ~3 positive Z-lobes across kVertTiles=1.5 that create horizontal altitude
    slabs when used for presence; Worley cells are spherical and do not share this slab problem.
  - **March steps are the primary quality lever from sea level.** With the default ~48 steps, each
    step is ~62.5 m on a vertical ray but far larger for oblique rays. The coarse sampling causes hard
    discontinuities between noise density levels, which project as discrete "oval slab" layers visible
    in debug mode 2. At **~150 steps** the sampling is fine enough to resolve continuous density
    gradients and clouds appear volumetric with no visible layers from sea level.
  - 150 steps is the **practical minimum** for glitchless clouds from ground level. The 512-step hard
    cap in the march loop provides headroom; the `marchSteps` UBO field exposes this as a slider.
    A default of 150 is recommended; lower values are acceptable from orbit where the shell is shallower.
- [x] **C8 — Cloud lighting.** Beer-Powder transmittance + dual-lobe HG + sun cone light-march (~6
  samples, reuse `optDepth` structure) + N-octave multi-scatter + sky ambient; front-to-back in-scatter/
  transmittance accumulation. *Done when:* lit tops, dark bases, silver lining, believable dawn/dusk/night.

  **C8 implementation (2026-07-01, session 19):**
  - **Altitude-stratified march stepping:** replaced `stepLen = (tExit-tEnter)/N` with
    `stepLen = (shellThick/N) / max(abs(dir.z), 0.02)`. Each step now advances exactly
    `shellThick/N` in altitude regardless of ray angle, making oblique-angle quality match
    vertical-ray quality without increasing step count.
  - **Spectral sun color:** `sunColorCloud` pre-computed once per cloudMarch call using
    `optDepth(p0, sunDir, tSA.y)` at the shell entry point. Clouds receive orange/red
    light at sunrise/sunset; night-side clouds (Earth's shadow check via raySphere) receive
    zero direct sun. `sunColorCloud` replaces the old implicit `vec3(1.0)` white.
  - **Night darkening:** `sampleDayness = clamp((dot(normalize(pECEF), sunDirECEF) + 0.15) / 0.3, 0, 1)`
    per sample gates the ambient sky dome term. Day ambient = blue-tinted `vec3(0.35, 0.55, 0.85)`;
    night ambient ≈ `vec3(0.001)`. Transition spans ±15° around the geographic terminator.
  - **City light upwelling:** at night (sampleDayness < 0.9) and cloud base (hNorm < 0.5),
    samples `earthNightTex` at mip 3 at the cloud's geographic position. After stripping
    0.06 baseline noise, contributes warm orange tint upward into overcast cloud undersides.
    Fades linearly to zero at hNorm = 0.5.
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
- [~] **C12 — Reflect-Orbital sky beams + shared cloud-shadow primitive.** Design revised and
  re-locked session 32 (see log below) — original stub above superseded. `GpuReflectBeams` SSBO
  (sector+atomicMax selection, same idiom as `sat_flare.comp`'s `flareEntries`/`sectorBright`) now
  exists and is written by `sat_orbit.comp`; a new shared `cloud_shadow.comp` low-res transmittance
  grid (128×128) now exists as the performant cloud-shadow primitive the user asked for, bundled
  into this step rather than built separately. **Steps 1-6 of 7 done** (data pipeline, shadow-map
  compute shader, general cloud shadow wired into terrain/ocean, beam volumetric in-scatter, beam
  ground-spot term, UI/settings) — all shipped as a first pass, not yet tuned in-app. **Step 7
  (profiling validation) remains**, and every gain/radius constant should be treated as a starting
  guess pending an in-app look. *Done when:* visible shafts from sunlit Reflect sats to night-side
  targets read as right after tuning, occluded by
  cloud/terrain, AND general cloud-shadow-on-ground is back (cheaply) without regressing the
  session-23 perf fix that removed the old per-pixel `cloudShadowFactor()`.

#### Phase E — Cirrus rework, Anvil, Airglow, Aurora (design locked 2026-07-12)

Takes priority over finishing C9/C11/C12 per the 2026-07-12 planning session — these four are
independent of composite/performance work and were sequenced ahead of it by user decision. Do
C13 → C14 → C15 → C16 in order; each is a self-contained session with its own "Done when" gate,
matching the C1-C12 convention. Airglow (C15) intentionally precedes Aurora (C16) to prove out the
"new emissive layer riding the N_VIEW loop" pattern on the simpler case before aurora's bigger
structural change.

- [x] **C13 — Cirrus volumetric rework.** Promote cirrus from the C4 flat 2D shell paste
  (`evalCloudLayer`, layer[1], ~11km) into the shell-march path (`cloudMarch`/`cloudDensity`), same
  architecture as the low cloud layer, so cirrus gets real depth/self-shadowing instead of a flat
  decal. Needs a wispier/more stretched noise than `cloudNoiseTex` provides (C6's volume is isotropic
  Worley — reads as blobby cumulus, not fibrous cirrus). Two paths, try (a) first:
  - (a) Anisotropic domain warp: stretch the UVW sampling coordinate along a wind-direction vector
    before sampling the existing `cloudNoiseTex` (binding 8). Shader-only, no new bake/binding.
  - (b) Fallback if (a) doesn't read as fibrous: bake a dedicated `cloud_noise_cirrus.comp` volume
    with elongated Worley cell jitter along one axis, mirroring the C6 `createCloudNoisePipeline`
    pattern exactly. Needs one new descriptor binding (next free slot = 10; 0-9 are all occupied).
  Fixed altitude shell confirmed (no change to shell-intersection geometry, only what feeds the
  density function). *Done when:* cirrus shows filament/streak structure from both ground and orbit
  altitude, no new layering artifacts.
  **Done (2026-07-12, session 21) — see session log for the architecture-mismatch discovery and
  final approach: a genuinely separate `cirrusMarch` function, not a second `cloudMarch` call.**

- [ ] **C14 — Anvil height-profile spread.** Tune the per-column height profile (`hFade`/`topFade`/
  `colH` in `sat_sky.frag:1097-1100`) so columns above a coverage/density threshold spread laterally
  near the top instead of tapering to zero at `colH` (blend in a lower-frequency/wider coverage sample
  as `hNorm → colH`). Threshold-gated — ordinary cumulus columns are untouched; only high-density
  "storm cell" columns get the anvil treatment. New `CloudParams` UBO fields: `anvilThreshold`,
  `anvilSpread` (mirrors the existing 10-slider pattern; persist via `settings.json`; add a settings-
  window slider for each). No new texture, no new binding. *Done when:* tall/dense columns visibly
  flatten and spread near the shell top; ordinary cumulus unaffected.

- [x] **C15 — Airglow (emissive N_VIEW layers).** Three altitude-banded emissive terms riding the
  existing `N_VIEW` atmosphere loop (`sat_sky.frag` ~lines 491-514) — same architectural slot as the
  still-unimplemented C10 city-upwelling term. Density per layer: `exp(-((h - peakAltM)/halfWidthM)^2)`,
  gated to night-side only (reuse the day/night dot-product test already used for cloud sun-visibility).
  Researched reference parameters (real airglow physics — use as defaults, tune from there):

  | Layer | Peak alt | Half-width (FWHM) | Color | Falloff character |
  |---|---|---|---|---|
  | Green (O I 557.7nm) | ~96 km | ~8-10 km | yellow-green | medium — dominant visible band |
  | Red (O I 630.0nm) | ~250-300 km | ~50-100 km | deep red | low/broad — diffuse halo above green |
  | Sodium (Na D 589.3nm) | ~90 km | ~5-8 km | orange-yellow | sharp/thin — keep brightness low relative to green |

  (OH Meinel hydroxyl ~87km is the physically strongest nightglow layer but is overwhelmingly
  near-IR — skip it for a visible-light renderer.) Time-domain banding: warp the horizontal sampling
  coordinate with a slow flow field (domain warp), not a simple scroll, to avoid a visible repeat
  period — reuse the technique already queued for cloud noise repetition (see "Pending after C6" /
  [[project-cloud-next-session]] item 1). Reuses `noiseTex` (binding 1) for the warp field; no new
  texture, no new binding. *Done when:* three independently-colored glow bands visible at night from
  ground and orbit, banding drifts/folds without an obvious repeat, zero cost impact in daylight.
  **Done (2026-07-12, session 22) — see session log for the altitude-range architecture mismatch
  (red band's peak sits outside the N_VIEW loop's ~100km ceiling — only green/sodium actually ride
  it) and the final per-band gain design.**

- [x] **C16 — Aurora (geomagnetic curtain primitive).** Most novel step — new geometry, likely a new
  emissive-only shell march (no Beer-Powder transmittance, additive glow only) distinct from
  `cloudMarch`. Centered on the **geomagnetic** pole, not geographic:
  - North geomagnetic pole ≈ 80.7°N, 72.7°W; south ≈ 80.7°S, 107.3°E (current epoch; drift
    ~0.05-0.1°/yr is negligible at sim epoch 2036 — a fixed ECEF constant is fine, no secular-
    variation model needed).
  Build one reusable **curtain primitive**: a band at a fixed angular offset (colatitude) from the
  geomagnetic pole direction, ripple-displaced along its own tangent via domain-warped noise (arc
  shape) plus a second finer octave for vertical striation (curtain folds). Future aurora types
  (diffuse patches, sharp arcs, substorm spirals) should be different noise configs of this one
  primitive, not new code paths — this generality was an explicit requirement.
  `stormStrength` slider (new `CloudParams`-style UBO field, 0-1) drives: oval equatorward expansion
  (wider colatitude band), brightness, and structure chaos (more warp octaves/amplitude at high
  activity). Occlusion: reuse the `raySphere` Earth-shadow self-occlusion test (same one used for
  cloud sun-visibility) so aurora doesn't render through the planet on the far side. Day suppression:
  gate on the **observer's local sky brightness** (`daySuppression`, same term used for satellite
  flare/glow), not sun-on-surface — aurora emission itself isn't sunlight-dependent, but the daytime
  sky background would drown it out, so it must be invisible from the ground in daylight. Depth:
  emissive-only, no depth *write* needed, but must respect the existing depth *test* so it doesn't
  draw in front of near geometry it shouldn't. Noise: try reusing `noiseTex` (binding 1) or
  `cloudNoiseTex` (binding 8) at a different scale for curtain structure before adding a new binding —
  only reach for a new slot (10, or 11 if C13(b) already claimed 10) if neither reads convincingly.
  *Done when:* a curtain-shaped glow band tracks the geomagnetic pole, intensity responds to the
  storm-strength slider, invisible at daytime ground level, doesn't render through the planet.
  **Done (2026-07-15, session 28) — see session log for the final design (deviated in a few
  places from the plan text above: day-gate uses `pc.sunDirENU.w` directly rather than a
  `daySuppression`-style term, and occlusion is handled by clipping the march against `tSurface`
  rather than a separate Earth-shadow test).**

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

### 2026-07-20 (session 32) — C12 revisited: Reflect-Orbital sky beams + shared cloud-shadow primitive (steps 1-2)

User asked to revisit the old C12 stub (written before the half-res `cloud_march.comp` split, baked
noise textures, and the session-29 profiling workflow existed) now that clouds/aurora share a
proven volumetric architecture — and flagged that general cloud-shadows-on-ground are an inevitable
visual requirement anyway, so the plan bundles **one shared, cheap cloud-transmittance primitive**
for both consumers instead of a beam-only one-off. Design discussed and locked in a planning pass
before any code; this session implemented the two lowest-risk, purely-additive steps (data pipeline
+ shadow-map compute shader) — no rendering behavior changes yet, build verified clean only (no
in-app look this session, per the "don't run the app" rule — that's the next session's job before
steps 3+ continue).

- **Ground-beam physics is NOT the same quantity as observer-perceived glint.** `sat_flare.comp`'s
  `mirrorPeak` is a view-dependent specular BRDF term (how bright the mirror looks to the
  *observer*); the ground spot a flat mirror casts is irradiance delivered to the target, which for
  a collimated flat reflector at these ranges does NOT fall off with sat-to-target range (the
  reflected beam's footprint tracks the mirror's own size, not an expanding point-source cone).
  Computed fresh in `sat_orbit.comp`: `groundIrradiance = SOLAR_CONSTANT(1361) * mirrorAreaM2 *
  mirrorFrac * max(0, dot(surfN0, sunDirECI))`, scaled by a new `beamGain` slider (default 1.0,
  no UI yet — settings wiring is step 6).
- **Why a sun-ward-only shadow lookup is a well-justified approximation for beam attenuation, not
  just a convenient one:** `sat_orbit.comp`'s existing target selection already picks the target
  MAXIMIZING `dot(satZenith, targetDir)` — i.e. the valid night-side target closest to the
  satellite's own nadir — so selected beams are already close to vertical as seen from the ground,
  making a sun-ward transmittance grid a reasonable proxy for the mirror's own (near-nadir)
  incidence angle.
- **`GpuReflectBeam`/`GpuReflectBeams`** (`SatelliteSim.h`): 32-byte entries (`satENU`, `intensity`,
  `targetENU`, `footprintRadM`), 16 azimuth sectors (binned by the TARGET's ENU direction, not the
  satellite's) — sector-stable selection via `atomicMax` + write-on-win, the IDENTICAL idiom
  `sat_flare.comp`'s `flareEntries`/`sectorBright` already uses (deliberately reused rather than
  inventing a new atomic-counter-append scheme — this codebase's `GpuGlowBuf.flareCount` field is
  explicitly dead/unused, confirming counter-append isn't the established pattern here).
- **New `reflectBeamsBuf`** (device-local SSBO): zeroed every frame via `vkCmdFillBuffer` (same
  pattern as `glowBuf`), written by `sat_orbit.comp`'s existing `TargetedReflector` branch (right
  after mirror-normal slew, using the already-selected `bestIdx`), gated on the ground TARGET
  clearing the observer's horizon (`elevCutoff`) — the actual binding visibility constraint, since
  the satellite side is almost always already above `elevCutoff` by the time execution reaches that
  branch, while two near-surface points are only mutually visible within a much tighter range.
  Added as **binding 4** on `sat_orbit.comp`'s descriptor set, **binding 10** on
  `cloud_march.comp`'s (future volumetric in-scatter consumer, step 4 — not yet wired), **binding
  17** on `sat_sky.frag`'s (future ground-spot consumer, step 5 — not yet wired). Each of the three
  shaders owns an independent `VkDescriptorSetLayout`/pool/set (confirmed no sharing exists in this
  codebase), so this was three separate C++ edits, not one.
- **New `cloud_shadow.comp`** — the shared cloud-shadow primitive. Fixed 128×128 dispatch,
  independent of screen resolution (no `onResize` handling needed), covering a flat tangent-plane
  ground grid centered on the observer (radius = new `cloudShadowRangeM`, default 80 km, no
  UI slider yet). Per-texel: reconstruct a sea-level ground point (accepted approximation — no
  `earthElevTex` binding needed, cloud-base altitude of 2+ km dwarfs most terrain relief), march
  toward the sun through the main cloud shell ONLY (`layers[0]`/`[1]`, matching the old removed
  `cloudShadowFactor`'s scope, not cirrus), accumulate transmittance, output a single-channel
  `R16_SFLOAT` texture. This directly replaces the old `cloudShadowFactor()` (removed session 23 as
  the dominant terrain/ocean cost — recovered from git history at commit `29e97b3^`: a live
  per-pixel raymarch, called on every screen pixel at full resolution) — the fix is not a cheaper
  march, it's the SAME march done once per low-res grid texel instead of once per screen pixel,
  consumed as an O(1) texture read (not wired to any consumer yet — that's step 3).
  - Ported (not shared via `#include` — this codebase has none) `cloudDensity()`/`cloudWarpOffset()`/
    `raySphere()`/`rotateZ()`/`remap()` from `cloud_march.comp`'s CANONICAL 3-slice version — NOT
    `sat_sky.frag`'s copy, which turned out to be **stale 2-slice dead code** (no call sites left
    since `cloudShadowFactor` was removed that same session-23 pass; worth deleting as unrelated
    cleanup sometime, not done this session).
  - Correctness requirement, not just performance: samples the exact same `cloudPhase`/
    `driftMult`/erosion parameters as the visible cloud render, so shadows track the clouds actually
    overhead as they drift instead of visibly detaching.
  - Ground point's `sunDirECEF` is derived in-shader from a `sunDirENU` push-constant field via the
    local ENU basis — mirroring `cloud_march.comp`'s own `main()` exactly, since there is no
    CPU-side `sunDirECEF` anywhere in this codebase to just pass in directly.
  - Works in true ECEF throughout (unlike `cloud_march.comp`'s view-ray march, which needs an
    artificial "obsPos" locally-rotated frame to share with the camera ray) — there's no camera ray
    here to share that convention with, so the ground point and every march sample are already
    real ECEF, one less transform than `cloudMarchCS`'s per-sample `pECEF = p.x*enuX+...`.
- **GPU timestamp pool grown 7→8 slots** (`VulkanContext::kTimestampCount`) for the new dispatch's
  own timing bucket (slot 2, "Cloud shadow map," inserted right after cloud march) — every
  downstream slot (orbit/flare/sky-bg/sat+star-draw/UI) renumbered by one; `gpuMsSmoothed` grew
  6→7. No spare slot existed to reuse (all 7 were already consumed).
**Follow-up (same day) — steps 3-6 implemented, in-app look after steps 1-2 confirmed no
regression (build normal, performance unchanged, as expected for pure plumbing):**

- **Step 3 — general cloud shadow wired into `directSun`** (`sat_sky.frag:1704-1708`, the exact
  historical `cloudShadowFactor` multiplication site): samples `cloudShadowTex` (new binding 18)
  at the hit point's own tangent-plane position (`hitPt.xy` — already the same observer-relative
  ENU offset convention the shadow grid was built from, confirmed exact rather than approximate).
  Fades to "no shadow" via a `smoothstep` on distance-from-grid-center instead of letting the
  `CLAMP_TO_EDGE` sampler repeat one boundary texel indefinitely past the grid's radius. New
  `SatDrawPC::cloudShadowRangeM` field (152 bytes total now) rather than growing the 3-way-shared
  `CloudParams` UBO for a field only `sat_sky.frag` needs. Gated behind new `debugDisableMask` bit
  256.
- **Step 4 — beam volumetric in-scatter in `cloud_march.comp`'s `B_total`:** deliberately NOT a
  per-march-step density sample (that would need 16 extra distance tests at every step of an
  already-hot loop) — instead a single per-pixel closest-approach test between the view ray and
  each of the ≤16 beam segments (standard two-line closest-point solve, clamped to the segment),
  gated to the cloud shell's altitude band. Real physical subtlety worth remembering: a volumetric
  light beam is only VISIBLE where there's scattering medium to catch it — invisible in fully
  clear air (nothing to scatter off) AND self-extinguished inside fully opaque cloud (the light
  can't reach the observer through it either) — visibility peaks at MODERATE cloud presence, not
  monotonically with more cloud. Modeled as a parabola (`4*v*(1-v)`, `v` = this ray's own
  already-computed `A_total` transmittance) rather than a fresh density lookup at the crossing
  point — cheap, reuses work already done. Gated behind new `debugDisableMask` bit 128.
- **Step 5 — beam ground-spot in `sat_sky.frag`:** placed after BOTH the terrain and ocean
  branches (not inside either) so it applies uniformly to whichever produced `surfColor` — Gaussian
  falloff by planar distance from `hitPt.xy` to the beam's `targetENU.xy` (both already the same
  ENU convention, no conversion needed), attenuated by sampling the SAME `cloudShadowTex` grid but
  at the TARGET's tangent-plane position rather than the pixel's own — this is a physically
  different quantity from step 4 (direct irradiance landing ON the ground, not in-scattering off
  atmospheric medium), so it does NOT get the peaks-at-moderate-cloud parabola; more cloud between
  mirror and target simply dims it monotonically, same as any other shadowed sunlight.
- **Step 6 — UI + persistence:** three new sliders on the Terrain tab (`Cloud shadow range (m)`,
  `Beam gain`, `Beam footprint (m)`) at slider idx 38-40, `settings.json` under the existing
  `"clouds"` section. Fixed the pre-existing `cloudBufs[33]` bug found while researching this same
  day (resized to `[41]` alongside `hovCloudMinus`/`hovCloudPlus`/`draggingCloud`). Two new
  `debugDisableMask` knockout checkboxes: "Reflect-Orbital beams" (128, checked in both
  `cloud_march.comp` and `sat_sky.frag` — one bit gates both consumers of the same feature) and
  "Cloud shadow map" (256) — the latter also gates the `cloud_shadow.comp` DISPATCH itself in
  `recordCompute` (not just its consumers), so its isolated cost reads as ~0 in the GPU frame
  breakdown when toggled off, not just "no visible effect."
- **Step 7 (profiling validation) — NOT done this session.** Everything above shipped as a
  first-pass and is expected to need visual tuning (glow radius, scale constants, footprint/range
  defaults) once seen in-app, same as every other volumetric feature's history in this file — no
  GPU-timestamp/knockout-based cost measurement taken yet. Do this before spending more tuning
  rounds on brightness, per the session-29 "measure, don't guess" lesson.
- Ordering gotcha hit and fixed during this pass: `createCloudShadowResources` (image/view/sampler
  only) had to move to run BEFORE `createGlowResources` in `init()` — the sky descriptor set's
  binding-18 write (built inside `createGlowResources`) needs `cloudShadowSampler`/`cloudShadowView`
  to already exist, same constraint `createCloudMarchResources`/`createAuroraNoisePipeline` already
  had a comment about for bindings 10/11/16.

**Follow-up #2 (same day) — first in-app look: shadow flicker + beams invisible.** User confirmed
shadows are "passable" but flicker/pop while moving, and reported seeing NO beams anywhere, even
standing where a Reflect Orbital's specular flare reads as focused on their position.

- **Shadow flicker root cause: classic moving-shadow-map texel aliasing, not a resolution
  problem per se.** `cloud_shadow.comp`'s grid recenters exactly on the observer's continuously-
  moving exact position every frame (`uv=0 -> enuZ*R_EARTH` by construction) — so a stationary
  cloud feature's mapping to a discrete texel index drifts sub-texel every frame as the observer
  moves, then that continuously-drifting sample gets written into a fixed texel, reading as
  "swimming"/popping. **Fix: texel snapping** (the standard CSM/shadow-map technique for this
  exact artifact) — quantize the grid's actual world-space center to whole multiples of
  `texelSizeM = 2*rangeM/128`, using `cityOffsetEastM`/`cityOffsetNorthM` (the pre-existing stable
  east/north accumulator the city-detail texture blend already relies on for the same "world-fixed
  offset" property) as the reference coordinate. The small residual gap (< 1 texel) between the
  snapped point and the observer's exact position is threaded through as a new field on BOTH
  `CloudShadowPC` (added to the grid-build offset) and `SatDrawPC` (subtracted before every UV
  lookup, both the general shadow term and the beam ground-spot target lookup) — computed once in
  `recordCompute()`, stored on `SatelliteSim` (`cloudShadowResidualM`), reused by `buildSatDrawPC()`
  so both push constants agree on the same frame's snap. `CloudShadowPC` grew 36→48 bytes,
  `SatDrawPC` grew 152→160. True cascade/mip-based improvement (the user's own "best" suggestion)
  remains a future upgrade — this fixes the flicker, not the fixed 128×128 resolution itself.
- **Beams invisible — one definite bug found and fixed, one design nuance clarified, root cause
  NOT fully confirmed without an in-app look at the fix.**
  - **Definite bug:** the volumetric sky-shaft term's cloud-shell gate tested only the SINGLE
    global closest-approach point between the view ray and the ~500+ km satellite-to-ground
    segment — landing that one point inside the ~km-thick cloud shell by chance is very unlikely,
    so the term almost never fired regardless of gain. Fixed: restrict the closest-approach search
    to the sub-range of the segment whose altitude (linearly interpolated between the satellite and
    target endpoints — segment altitude decreases ~monotonically end to end, so this is a good
    approximation without a real root-find) actually falls inside `[cloudBaseAlt, cloudTopAlt]`,
    then search only within that sub-range instead of the whole segment.
  - **This bug does NOT explain the ground-spot term also being invisible** (no altitude gate
    exists there) — that points at either the write side never firing, or the two conditions the
    user conflated genuinely being different: sat_flare.comp's specular glint brightness depends on
    `dot(reflect(-sunDir, surfN0), directionToObserver)`, which by the mirror-reflection identity
    equals `dot(toTarget, directionToObserver)` — a bright flare requires the OBSERVER to sit near
    the same direction-from-satellite as the chosen TARGET, which for this satellite's very tight
    specular cone (`mirrorExp` ~60000, half-width matching the ~0.26° solar disc) should in practice
    require the observer to be within roughly a few to a few dozen km of the real target — so a
    genuinely bright flare SHOULD imply a nearby target in this sim's own math, but this chain of
    reasoning hasn't been verified against an actual in-app case yet.
  - **Added a diagnostic instead of continuing to guess:** `reflectBeamsBuf` changed from
    device-local to HOST_VISIBLE|HOST_COHERENT (same reasoning as `glowBuf`), read back one-frame-
    stale each frame in `recordCompute()` into two new members, `lastActiveBeamCount`/
    `lastNearestBeamDistM`, displayed as a new "Active beams / nearest" row in Settings → Display
    right below the knockout toggles. This directly answers "is `sat_orbit.comp` writing ANYTHING,
    and how far is the closest one" independent of whether the render itself is visible — the next
    concrete step once the user looks again.

**Follow-up #3 (same day) — root cause found via the new diagnostic: a pre-existing, unrelated bug.**
User reported the diagnostic read 0/None literally everywhere on Earth, including while standing
in a Reflect Orbital's specular flare region — ruling out "just rare by design" and pointing at the
write side never firing at all.

Found in `updatePositions()` (`SatelliteSim.cpp`, ~line 4791-4797), **predates C12 entirely** —
this is the actual reflector-target generation code the whole `TargetedReflector` attitude mode has
depended on since it was built, not anything touched this session until now:
```cpp
reflectorTargetsECEF[kNumReflectorTargets - 1] = glm::normalize(glm::vec3(0.2527f, -0.4596f, -0.8205f)); // correct: 67°S/67°W spawn point
// Zeroth slot
// Antartic Station
reflectorTargetsECEF[kNumReflectorTargets - 1] = glm::normalize(glm::vec3(0, 0, -1.0)); // BUG: same index again
```
The comment says "Zeroth slot" but the code writes to `kNumReflectorTargets - 1` (the LAST slot)
**again** — immediately clobbering the just-set, CLAUDE.md-documented "fixed target at the observer
spawn point" with `(0,0,-1)`, the geographic South Pole ECEF direction. 67°S/67°W is ~23° of
latitude (~2555 km) from the true pole — so the one target explicitly designed to guarantee a
nearby aim point was silently ~2555 km from the observer's spawn the whole time, and the other 200
random targets are sparse enough across Earth's full surface (expected nearest-neighbor distance
~1600 km) that essentially none will ever land within local horizon range by chance. Index 0 itself
is left at its zero-initialized default (the generation loop above deliberately starts at `ti=1`,
confirming index 0 was reserved for something — most likely the "Antarctic Station" idea the
comment names, just written to the wrong index) — harmless (produces a permanently-invalid,
unused slot) but still an incomplete thought, not touched further.

**Fix:** removed the erroneous second assignment, restoring the documented spawn-point target.
A real "Antarctic Station" fixed target, if still wanted, belongs at index 0, not
`kNumReflectorTargets - 1` — not implemented here since there's no other reference to that idea
anywhere in this file or CLAUDE.md to confirm intent.

**Expectation to set for next in-app check:** this restores exactly ONE reliable near-observer
target (at the 67°S/67°W spawn point, valid only while that point is on the night side) — it does
NOT make the other 200 random-global targets any less sparse. Seeing a beam still requires either
being at/near the spawn point during local night, or waiting for a Reflect Orbital satellite's own
nearest-neighbor scan to happen to land near wherever the observer currently is. Watch the "Active
beams / nearest" readout, not just the render, to confirm this landed.

**Follow-up #4 (2026-07-21) — cloud shadows confirmed fixed by texel snapping. Beams: user got in
range of one and found it "extremely glitchy and non-persistent," appearing to come from a
different satellite than the one whose flare was actually bright — a second, real bug, this time
genuinely in this session's own C12 code, not a red herring.**

**Root cause: azimuth-sector keying was the wrong tool for this buffer.** `ReflectBeamsBuf` used
16 azimuth-from-observer sectors (borrowed directly from `GpuGlowBuf`'s `flareEntries`/
`sectorBright` idiom in `sat_flare.comp`) — that scheme exists there to dedupe potentially
thousands of satellites across the WHOLE sky into a handful of render slots, a genuinely different
problem. Here, each sector spans 22.5° of compass bearing from the observer — so **two entirely
different satellites aiming at two entirely different real-world targets only need to share the
same 22.5° wedge to collide**, and whichever one's computed ground irradiance was higher would win
the slot via `atomicMax`, silently evicting the other. That's exactly what was reported: the
brightest satellite's beam lost a bucket collision to some unrelated satellite whose target
happened to share its bearing, and if their relative brightness was close, the winner could flip
frame to frame — reading as "glitchy, came from the wrong satellite."

**Fix: key by TARGET IDENTITY instead of bearing.** Every satellite's nearest-target scan already
produces `bestIdx`, a stable index (0-200) into the fixed 201-entry `reflTargets[]` array — i.e. a
real, semantically-meaningful identifier for "which of the 201 possible ground locations is this."
Indexing `ReflectBeamsBuf` directly by `bestIdx` (renamed `BEAM_SECTORS`→`BEAM_SLOTS`, 16→201, and
`sectorBright`/`beams[sector]`→`slotBright`/`beams[slot]` throughout `sat_orbit.comp`,
`cloud_march.comp`, `sat_sky.frag`, and `SatelliteSim.h`'s `GpuReflectBeams`) makes cross-target
collisions structurally impossible — two DIFFERENT targets can never share a slot, full stop. Two
satellites aiming at the SAME target still correctly merge via `atomicMax` (brightest wins) — the
only case where merging is actually legitimate. `GpuReflectBeams` grew 576→7236 bytes (trivial for
a GPU buffer); the per-pixel consumer loops grew from 16 to 201 early-exit iterations — worth
checking under the eventual step-7 profiling pass, but not expected to be significant given the
branches are cheap and the vast majority of slots are empty at any moment.

**Not yet re-confirmed in-app.**

**Follow-up #5 (2026-07-21) — re-architected from OBSERVER-referenced to SITE-referenced, per the
user's own diagnosis.** After the target-identity fix, user reported: a vague soft "white zone"
near targets, beams reading as coming from the wrong sky position ("opposite of the satellites"),
glitchy ones on the horizon, and — the key diagnostic clue — **climbing above just ~300m of
altitude made every beam vanish entirely.** That last symptom pointed straight at the remaining
`targetSinEl > pc.elevCutoff` write-gate: a razor-thin, continuously-recomputed spherical horizon
test is exactly the kind of thing a few hundred meters of altitude can flip. User's proposed fix:
stop gating on the OBSERVER's relationship to a site at write time — precompute which of the ~200
fixed sites are currently serviceable (on the night side, matched to a satellite) independent of
where the observer is, then only use the observer's position as a RENDER-time "am I close enough to
this already-known-active site to bother drawing it" filter. Agreed this was the correct fix, not
just a workaround.

**What made this a small, targeted change rather than a rewrite:** the O(satCount × 201) nearest-
target scan this depends on already exists and already runs from EVERY satellite's own perspective
regardless of the observer (that's how `bestIdx` gets picked at all) — nothing new needed there.
The blocker was purely the leftover observer-relative gate on top of it, which only mattered
because a satellite ALSO has to already be visible above the OBSERVER's own orbital horizon to
reach this code in the first place (an earlier, unrelated occultation cull returns early
otherwise) — and LEO horizon distance (~2000+ km) is already generously larger than any range this
effect could ever render something at, making it a sufficient outer bound on its own. So the fix
was: **delete the target-horizon gate in `sat_orbit.comp`** (every satellite reaching the
`TargetedReflector` branch with a real target now registers into its site's slot unconditionally),
and **add an explicit `kBeamMaxRangeM` (500 km) render-time distance cutoff** in both consumer
loops (`cloud_march.comp`, `sat_sky.frag`) as the new, smooth "is the observer in range of this
site" check — a plain 3D distance test with no altitude sensitivity, replacing the fragile spherical
horizon calculation. Expected to also fix the other three symptoms as a side effect: with each site
now getting its own dedicated, uncontested slot (no more racing for the handful that used to pass
the strict gate), there's no more competition-driven flicker or wrong-satellite substitution.

**Scaling questions raised by the user, addressed without code changes (analysis only):**
- *Can this handle scaling Reflect Orbital from 1,000 satellites (current) to 5,000 (the real
  constellation's documented plan)?* Yes, with no expected performance concern and no architecture
  change needed. The per-satellite target scan already costs O(satCount × 201) regardless of the
  observer-gate fix — going 1,000→5,000 satellites is a 5× increase in a workload this engine
  already runs 6-30× larger for other constellations in this same sim (Starlink Gen2 alone is
  30,480 satellites, already well above `MAX_SATELLITES` headroom). More importantly, the
  CONSUMER-side render cost is bounded by `BEAM_SLOTS` (201, fixed) regardless of satellite count,
  since multiple satellites servicing the same site just resolve via `atomicMax` into one slot —
  so rendering cost literally does not change with constellation size at all.
- *Future: replace the 201 procedurally-random target sites with real-world locations (e.g. an
  actual list of solar farms)?* Fully compatible with this architecture as-is — it only changes
  where `reflectorTargetsECEF[]`'s values come from (a curated list instead of `rand()`), nothing
  about the site-referenced selection/rendering pipeline. Not implemented now; revisit whenever
  wanted.

**Not yet re-confirmed in-app.**

**Follow-up #6 (2026-07-21) — the site-referenced re-architecture worked: user confirmed beams and
ground-spots now display.** Remaining complaints: cloud illumination reads as "very intense and
sharp," a hard-edged region of lit vs. dark cloud rather than a soft glow; the lighting pattern
"glitches and flips" when passing through a beam. Two genuinely different root causes, both in
`cloud_march.comp`'s volumetric term, both fixed:

- **The flip: a real numerical singularity, not a coordinate bug.** The closest-approach-between-
  two-lines solve (`sSeg = (eDot - bDot*dDot) / denom`, `denom = 1 - bDot²`) has a textbook
  removable singularity as the view ray becomes near-parallel to the beam (`bDot -> ±1`) — which is
  exactly the geometry of "looking straight through the beam," the most likely case for a user to
  actually encounter. The numerator doesn't vanish at the same rate as `denom`, so the division
  swings wildly right in that regime, snapping the glow to a very different position frame to
  frame. Fixed by falling back to the shell-crossing sub-range's midpoint whenever `denom` drops
  below a threshold (`1e-3`) instead of dividing — correct in that regime too, since "nearly
  parallel" means the perpendicular distance barely varies along the sub-range anyway, so which
  point you pick barely matters.
- **The sharp edge: mostly the hard altitude-band clamp, likely compounded by tonemap saturation.**
  The shell-membership test clamped the search to `[sLow, sHigh]` with no transition — the closest-
  approach point could snap to that window's exact boundary as view direction changed, a genuine
  discontinuity in `perpDist` right at the edge. Added a `kAltMarginM=2000` fade zone (smoothstep,
  same shaping style the main cloud march already uses for its own `hFade`) instead of a binary
  window. Also widened `kBeamGlowRadiusM` 3000→7000 m (spread the same energy over more area) and
  roughly halved `kBeamScale` 3e-8→1.5e-8 to compensate and pull back from what was likely tonemap
  saturation turning a smoothly-decaying signal into a "pure white here, dark there" look. Also
  added a smoothstep fade at the glow radius's own outer tail (was a hard `continue`) for full
  continuity. All first-pass constants — expect another round once seen in-app, same as every
  other volumetric feature here.

**Godrays / ground-mist question — answered, not implemented.** User asked whether "cloud lighting
fixed enough to show illuminated mist/dust near the ground below the cloud layer" is just a
godrays/crepuscular-ray effect that should be generalized to clouds overall, not kept Reflect-
Orbital-specific. Answer: yes, conceptually — and this project already has a matching, unbuilt
backlog item for exactly that shared medium: **C11 ("Fog / dust / haze")** in the Phase D backlog
above, explicitly scoped as "the low medium beams scatter through" for any strong local light
source, not just one. Recommended NOT building a bespoke reflect-beam-only ground-mist hack in this
pass — a proper shared low-altitude medium (sun, moon, city lights, reflect beams all scattering
through the same fog layer) is more visually coherent and avoids building two separate
approximations of the same phenomenon. This is a substantial standalone feature (its own low-
altitude march or a real extension of the existing shell-march machinery down near the ground) that
deserves its own scoping/design pass rather than being folded into this fix-up session — not
started, awaiting a decision on when to pick up C11 properly.

**Two new settings-tunables added (Terrain tab):**
- **"Beam max range (m)"** (idx 41, `beamMaxRangeM`, default 500 km, range 50 km-2000 km) — the
  render-time "is the observer close enough to this site" cutoff introduced in follow-up #5 was a
  hardcoded constant in both consumer shaders; now a single CPU-side value threaded through both
  `CloudMarchPC` and `SatDrawPC` (grew 132→136 and 160→164 bytes respectively).
- **"Beam footprint (m)"** (idx 40, `beamFootprintRadM`) slider minimum lowered 1000→500 m (step
  also 500) per user request, for a tighter, more spot-like ground glow than the 50 km default
  allowed dialing down to before.

**Also fixed:** a documentation-only slip from an earlier follow-up in this same session — the
"### 2026-07-20/21 (session 31)" header above this entry had been accidentally deleted while
splicing in an earlier follow-up's content (already present in the user's last commit, "Kind of
beam"). Restored; no code was affected, only this log's own structure.

**Follow-up #7 (2026-07-21) — found the REAL cause of the "dark spot directly over the site" and
most of the remaining "odd rotation" reports: follow-up #6's near-parallel fallback only patched
half of the unstable computation.** `sSeg` (which point along the beam segment) got a safe
fallback when `denom` was small, but `tRay` (where along the VIEW RAY the closest approach sits)
was still computed via the same coupled formula, `tRay = sSeg*bDot - dDot`. `dDot = dot(dir, obsPos
- satWorldPos)` scales with the FULL satellite-to-ground distance — hundreds of km — so for an
observer standing at/near a site and looking UP at the sky (dir nearly anti-parallel to segHat,
exactly the most natural way to look at "the beam overhead"), `tRay` came out close to the full
segment length, projecting the ray-side comparison point out near the SATELLITE's own altitude
instead of the few-km-thick shell actually overhead. `perpDist` then measured as enormous and the
term got culled — a dark hole in exactly the spot that should be brightest.

**Fix: stopped solving the coupled two-line system for `tRay` at all.** Once `sSeg` picks a
representative point on the segment (`closestSegPt`), finding that FIXED point's distance to the
ray is a plain projection — `tRay = dot(closestSegPt - obsPos, dir)` — a single dot product with
no division, so it cannot blow up regardless of how parallel `dir` and `segHat` get. This is a
strictly more robust formulation than the original "true joint closest-approach-between-two-lines"
solve for both unknowns simultaneously; it gives up a small amount of optimality in normal
(non-parallel) viewing angles in exchange for being well-defined in every case, which is the right
trade for a per-pixel visual term where "occasionally not the mathematically exact closest point"
is invisible but "occasionally a wild division blowup" is a black hole in the sky.

**Not yet re-confirmed in-app.** Still pending, raised in the same report and not yet addressed in
code: (a) whether any residual "odd rotation" persists once this lands (some may simply be
`phaseCloud`'s real forward/backward scattering anisotropy, which SHOULD look different from
different angles — worth distinguishing from an actual bug once seen), and (b) the "one beam per
site, even when many satellites service it" limitation — see the "site capacity" discussion below,
answered as a design question, not yet implemented.

**How beam selection actually works, for reference (user asked directly):** a satellite's target
choice (`bestIdx`) IS its orientation — `sat_orbit.comp` scans all 201 possible sites and picks
whichever maximizes `dot(satZenith, targetDir)` among currently-valid (night-side) ones, then
slews its mirror normal toward that exact target via the sun/target half-vector. `ReflectBeamsBuf`
is keyed by that same `bestIdx` (site identity, follow-up #5), one slot per SITE, not one per
satellite. When multiple satellites simultaneously choose the same site (a well-serviced site),
`atomicMax` on intensity keeps only the single brightest one — every other satellite servicing that
same site loses the race and is silently dropped, which is exactly the reported "only one beam for
what should be a whole sky's worth of satellites." This is a structural consequence of the
one-slot-per-site design (chosen specifically to make cross-site collisions impossible, follow-up
#5), not a separate bug — supporting multiple simultaneous beams per site needs a deliberate
capacity decision (how many slots per site) with real buffer-size/loop-cost tradeoffs, raised with
the user rather than picked unilaterally.

**Follow-up #8 (2026-07-21) — multi-beam-per-site capacity implemented, user chose 4 per site.**
`kBeamSlotsPerSite = 4` (new constant, `SatelliteSim.h`/`sat_orbit.comp`/`cloud_march.comp`/
`sat_sky.frag`, all four must stay in sync). `ReflectBeamsBuf` grew from 201 slots to 201×4=804
(`kBeamSlots` now derived as `201 * kBeamSlotsPerSite` rather than hardcoded); buffer size
~6.4KB→~25.7KB, consumer loops 201→804 early-exit iterations/pixel — both trivial on a GPU.

**Sub-slot assignment:** `sat_orbit.comp` hashes each satellite into one of the 4 sub-slots via its
own `gl_GlobalInvocationID.x` (`i % BEAM_SLOTS_PER_SITE`) — stable frame to frame per satellite (a
given satellite's invocation index doesn't change), so the hash itself introduces no flicker.
`slot = bestIdx * BEAM_SLOTS_PER_SITE + (i % BEAM_SLOTS_PER_SITE)`. `atomicMax` still applies
within each sub-slot for the (now much rarer) case of a 5th+ satellite landing on the same site
AND the same sub-slot — previously every 2nd+ satellite on a site collided, now only roughly every
5th does, assuming reasonably uniform hash distribution across servicing satellites.

**Not yet re-confirmed in-app.** Consumer shaders needed no logic changes beyond the constant —
they already loop over every slot in `[0, BEAM_SLOTS)` generically, unaware of the site/sub-slot
grouping, so growing `BEAM_SLOTS` alone was sufficient there.

**Follow-up #9 (2026-07-21) — 4-per-site rejected by the user; replaced with an uncapped
atomic-append design.** User re-tested and reported the dark spot again, PLUS explicitly rejected
the whole site-slot-capacity direction: "4 satellites per site is an awful limitation for a site
potentially serviced by dozens of satellites... the beam will always be glitchy as it chooses
which satellite it wants to originate from." Correct call — ANY fixed small-N-per-site scheme has
this problem by construction: whenever more satellites are eligible than slots, WHICH ones
currently hold a slot keeps changing as brightness/geometry shifts, and that churn is visible
regardless of whether N is 1, 4, or 8.

**New design: no site keying, no deduplication, no arbitration at all.** Every satellite that
reaches the `TargetedReflector` branch with a real target claims its own slot via a single global
`atomicAdd` counter (`beamCount`) and writes unconditionally — there is nothing to compete over, so
nothing can glitch between representations. `kMaxActiveBeams = 256` is pure headroom, not a
"keep-the-best-N" cap: a satellite must already clear the OBSERVER's own orbital horizon to reach
this code at all (~2,000+ km radius, enforced earlier in `sat_orbit.comp` for an unrelated reason),
which bounds how many can be simultaneously eligible. Rough estimate: horizon-visible fraction of
Earth's surface from a ~500 km-altitude LEO horizon ≈ (π·2000²)/(4π·6371²) ≈ 2.5%; at 1,000
satellites that's ~25 simultaneously eligible, ~125 even at the real constellation's planned 5,000
— both comfortably under 256. `GpuReflectBeams` restructured: `uint32_t beamCount` + 12 bytes
padding (std430 alignment for the following array-of-16-byte-aligned-struct) + `entries[256]`
(8,208 bytes total, still trivial). Consumer shaders now loop `min(beamCount, kMaxActiveBeams)`
instead of a fixed count — cheaper in the common case (typically far fewer active beams than 256)
instead of always paying for the old fixed total. `sat_orbit.comp`'s writer collapsed to 4 lines
(`atomicAdd` + bounds check + unconditional write) — simpler than every previous scheme.

**Follow-up #10 (2026-07-21) — found the REAL root cause of the dark spot, which survived TWO
rounds of patching: catastrophic floating-point cancellation, not a logic bug.** The
closest-approach formula's `eDot`/`dDot` terms are dot products built from
`w0 = obsPos - satWorldPos` — and `satWorldPos` sits up to ~2,000 km from `obsPos` (satellite
altitude + slant range). Extracting a small (sub-km) answer by subtracting two independently-
computed LARGE-magnitude float32 quantities (`eDot - bDot*dDot`) is exactly the class of bug this
codebase's `raySphere()` was rewritten to avoid at R_EARTH scale (`TERRAIN_PLAN.md` session 22) —
the earlier "near-parallel fallback" and "stable tRay projection" fixes each patched one symptom of
this same underlying cancellation without addressing the cancellation itself, which is why the
dark spot came back after both.

**Fix: stop solving for a ray-dependent "optimal" point on the segment at all.** The shell-crossing
sub-range is only a few km long — thinner than `kBeamGlowRadiusM`'s own spatial support — so ANY
point within it is close enough to any other for this term's purposes. Now unconditionally uses
the sub-range's midpoint (`satWorldPos + 0.5*(sLow+sHigh)*segHat`) as the reference point, with
`tRay` still found via the stable direct projection from follow-up #7. This removes every
large-minus-large subtraction from the hot path entirely — not a better patch, a different and
much simpler formulation that was never going to be needed after this if the fixed-midpoint
approach had been the starting design instead of the "true closest approach" one. Not yet
re-confirmed in-app.

**Cloud shadow "pixelated again" — raised same message, not yet diagnosed.** No code in
`cloud_shadow.comp` or the texel-snapping path (follow-up #5's fix, user-confirmed working) was
touched in follow-ups #6-#10 — all of that work was scoped to the beam terms. Leading hypothesis:
the "Cloud shadow range (m)" slider may have been adjusted upward during the same testing session
(a larger range at the grid's fixed 128×128 resolution directly coarsens meters-per-texel — an
inherent trade-off of the fixed-resolution design, not a regression) — raised with the user rather
than assumed, since nothing in this area's code changed.

**Follow-up #11 (2026-07-21) — the atomic-append redesign had its own bias, found from a very
specific report: satellites sweeping left-to-right had their flares "fade in from the left and
stack up on the right," reversing when time reversed, and lagging when moving quickly into a new
region.** Root cause: `kMaxActiveBeams` (256) was too tight for the real geometry. Reflect Orbital
is a "Disk" constellation — a SINGLE orbital plane with 10 concentric altitude rings, not spread
across many planes/RAANs — so the whole constellation sweeps together along essentially one great
circle. From a fixed observer, the visible arc of that one ring is a large, non-scattered fraction
of the total constellation (re-estimated ~900 simultaneously eligible at the real 5,000-satellite
scale — the earlier uniform-sphere estimate in follow-up #9, ~125, assumed the WRONG distribution
shape entirely). When eligible count exceeds the cap, only the first N to win the `atomicAdd` race
get written — and race order correlates with GPU dispatch order, which correlates with satellite
ARRAY index, which (for satellites all in one swept plane) correlates with WHERE ALONG THE RING
each one currently sits. That is not a random 5% miss; it's a systematic, position-correlated
exclusion — exactly "one side of the sky always loses," and it flips when the sweep direction
flips. **Fix:** raised `kMaxActiveBeams` 256→2048, comfortable headroom over the re-estimated
worst case, making overflow (and therefore this whole class of bias) effectively never happen
rather than trying to make an overflow's bias smaller or fairer. `GpuReflectBeams` grew
accordingly but is still trivially small for a GPU buffer.

**Follow-up #12 (2026-07-21) — two more issues from the same report, plus a requested debug tool.**

- **Cloud lighting direction was backwards.** User: looking FROM a site TOWARD the flaring
  satellite lit clouds up fully; looking DOWN at a site from within the beam left clouds
  edge-lit/backlit only — the opposite of both views' expected brightness. Root cause: the
  volumetric term's phase function was called as `phaseCloud(dot(dir, segHat))`, where `segHat`
  points satellite→target (the direction light PROPAGATES). Every other `phaseCloud()` call site
  in this file (the sun's own scattering term) uses a vector pointing TOWARD the light source
  (`sunDir`), so `cosA=+1` means "looking toward the source" and lands in the strong forward-
  scattering lobe — matching the real "bright aureole around the sun seen through haze"
  phenomenon. Passing `segHat` directly used the opposite sign convention, silently swapping which
  viewing direction got the strong vs. weak lobe. Fixed: `phaseCloud(dot(dir, -segHat))`.
- **Is the ground-illumination flicker linked to the existing lens-flare selection flicker?**
  User asked directly. Answer: NOT the same code — lens flare (`sat_flare.comp`'s pre-existing
  `flareEntries`/`sectorBright`, 8 azimuth sectors) is a completely separate buffer/shader from
  `ReflectBeamsBuf`, untouched by any C12 work. They ARE the same CLASS of bug (arbitration among
  more candidates than available slots causes visible switching when relative rankings shift) —
  follow-up #11 fixed that class of bug for beams specifically by making overflow not happen; the
  lens-flare system's own 8-sector cap is a separate, longer-standing case of the same pattern,
  out of scope for C12, not touched here.
- **Debug pointing-ray visualization, as requested.** New `GpuReflectBeam.reflectDirENU` field —
  the mirror's ACTUAL current reflected-sunlight direction (`reflect(-sunDirECI, surfN0)`), which
  can differ from the straight satellite→target line while `MIRROR_ROT_RATE`-limited slewing is
  still catching up (useful precisely for seeing that lag). New opt-in-only toggle
  (`showBeamDebugRays`, Settings → Display "Show beam pointing rays" checkbox) draws each active
  beam's actual pointing direction as a long (1000 km), fat (15 km radius — deliberately much
  bigger than the real glow's 7 km), green line, so convergence at a busy site is directly
  visible. Deliberately NOT part of `debugDisableMask` — that mask means "disable this normally-on
  thing" (0 = normal); this is the opposite shape ("enable this normally-off extra"), so it's its
  own field on `CloudMarchPC` rather than an overloaded bit. Rendered via 32 samples along the
  ray's length (not the single-fixed-point trick the real glow uses — that only needs to cover a
  narrow few-km shell band, but a multi-hundred-km debug ray needs to look continuous from many
  viewing angles), each sample still using the safe fixed-point-projection technique from
  follow-up #10 (no closest-approach solve built from large-magnitude `obsPos`/`satWorldPos`
  differences). `GpuReflectBeam` grew 32→48 bytes for the new field.

**Not yet re-confirmed in-app** for any of the above (cap increase, phase sign fix, or the new
debug tool). Cloud shadow pixelation question from follow-up #10 also still open.

**Follow-up #13 (2026-07-21) — phase-sign fix alone didn't resolve the lighting-direction report;
debug rays needed thinning + ground-tracing.**

- **Cloud lighting still reported reversed** ("as if illuminated by the site itself instead of
  the satellites from above") even after follow-up #12's phase-sign fix. Reconsidered and now
  believe `beamMediumFactor` (the `4*v*(1-v)` parabola on cloud transmittance) was the more likely
  dominant cause all along, not the phase function's sign. It uses the CURRENT VIEW RAY's own
  aggregate transmittance (`A_total`) as a stand-in for "is there scattering medium at the beam's
  actual 3D location" — a proxy that can be very wrong, since the viewer's own line of sight and
  the beam's position are generally different rays through different parts of the sky. The old
  shape went to exactly ZERO at both fully-clear and fully-opaque, so a view ray happening to look
  through clear air killed the glow regardless of the beam's real surroundings, and a view ray
  through dense cloud did the same — plausibly explaining "lit entirely" vs. "backlit" as an
  artifact of which UNRELATED transmittance the two viewing positions' own rays happened to
  sample, not any true directional lighting relationship. Fixed by giving it a floor
  (`mix(0.35, 1.0, ...)` instead of the raw parabola), so the glow no longer disappears based on
  this proxy's accuracy. Also damped the phase function's forward/backward swing toward uniform
  (`mix(1.0, phaseCloud(...), 0.35)`) — the sign is still believed correct, but a ~3x swing was
  likely amplifying any OTHER inconsistency into a stark, confusing "reversal" on top of it.
  **Genuinely uncertain this fully resolves it** — flagged as such rather than asserted, given the
  first attempt (sign-only) didn't land; a real fix may need sampling actual `cloudDensity()` at
  the beam's own location instead of reusing the view ray's transmittance, which is a bigger
  change not attempted yet.
- **Debug rays too thick/bright to distinguish, and stopped mid-atmosphere instead of reaching the
  ground.** Both straightforward: radius 15,000m→2,500m and scale 0.12→0.03 (thinner, dimmer, so
  convergence at a busy site reads as many distinct lines instead of one blown-out mass); length
  changed from a fixed 1,000 km to the ACTUAL ray-sphere intersection with Earth (`raySphere`
  against `R_EARTH`, same helper the rest of this file already uses), so each ray now traces to
  where it truly reaches the ground regardless of the satellite's altitude or exact aim, with a
  3,000 km cap only as a fallback for a ray pointing away from Earth entirely.

**Not yet re-confirmed in-app.**

**Follow-up #14 (2026-07-22) — replaced the proxy-based glow with a real per-sample march,
following the user's own debug-ray-derived design.**

User confirmed the re-tuned debug rays now look correct ("well sized glowing green beams going
straight from the satellites to the ground"), but flagged a new artifact: visible discrete/dotted
stepping near the ground, from the debug ray's fixed 40-sample count spread over a now-variable
(actual ground-intersection) length. Rather than just fixing the dotting, the user proposed a
bigger pivot: base the REAL cloud/terrain illumination on the same marching technique the debug
rays use, instead of continuing to patch the proxy-based approach — "optimize the steps close to
the ground and not incorporate anything outside of the atmosphere... if we did this march in the
cloud shader, we could also incorporate the lighting and occlusion better."

Implemented for the cloud volumetric term (terrain ground-spot term intentionally left on its
existing `cloudShadowTex`-based approximation for now — see scope note below):

- New `beamCloudDensityAt(p, enuX, enuY, enuZ, hNorm)` function (right before `main()`) ports the
  canonical per-step column evaluation from the main march (`pECEF`/lon-lat/`pUV`, `localCov` via
  `earthCloudsTex`, `colH`/`baseH`/`hFade` shaping, `cloudDensity()`) so a beam's brightness comes
  from what's ACTUALLY at its own 3D location — directly replacing the old `beamMediumFactor`
  proxy (the current view ray's unrelated transmittance) that follow-up #13 suspected but wasn't
  certain about.
- The main beam block now marches along `reflectDirENU` (the ACTUAL current reflection direction —
  the same vector the debug rays use and the user confirmed looks right) instead of the
  `satWorldPos→targetWorldPos` idealized segment. Physically more correct: real light follows where
  the mirror actually currently points, not its intended aim point during slew.
- Two-phase per beam to stay affordable at up to `BEAM_MAX_ACTIVE` (2048) candidates: a cheap
  single-point proximity pre-check (same fixed-midpoint trick from follow-up #10, still avoiding
  the `obsPos`/`satWorldPos` cancellation bug) rejects beams nowhere near this pixel's view ray;
  only survivors pay for the real 20-sample density march.
- The march covers only the cloud shell's own altitude band (a few km), found by linearly
  interpolating between the atmosphere-entry point (exactly `R_ATMOS`, altitude 100,000m by
  construction) and the ground-hit point (exactly `R_EARTH`, altitude 0m by construction) — cheap
  (two `raySphere` calls, no density evaluation) and exact for a near-vertical beam. This is both
  the accuracy fix and a perf win: the vast majority of a satellite's real distance is vacuum with
  nothing to evaluate, so a fixed step budget spent only on the shell crossing lands where cloud
  can actually be, instead of being wasted on hundreds of km of empty atmosphere in between.
  Front-to-back transmittance (`T`) attenuates deeper samples, so dense cloud near the shell top
  correctly self-shadows what's beneath it along the beam — "handle volumetrics... if we play it
  right," per the user's own framing.
- `altFade`/`beamMediumFactor` removed outright (no longer needed — real per-sample density
  naturally goes to zero outside actual cloud, and `cloudDensity`'s own `hFade` already provides
  soft column edges). `kBeamScale` reset to a first-pass guess (`4e-8`) since the brightness
  formula changed shape entirely (accumulated real in-scatter vs. a single fixed-proximity term) —
  **expect this needs retuning in-app**, same as every other beam constant so far.
- **Scope decision:** only the cloud volumetric term was rebuilt this round. The ground-spot term
  in `sat_sky.frag` still uses the existing `cloudShadowTex` grid lookup for its cloud attenuation
  — plumbing this new beam march's result across shaders (`cloud_march.comp` doesn't currently
  have write access to `ReflectBeamsBuf`) is a bigger follow-up if wanted, not attempted here.

Builds clean (`cmake --build build`, `cloud_march.comp` compiles via glslc). **Not yet seen
in-app** — brightness scale, march step count, and whether this actually resolves the
lighting-direction complaint all need the user's own look.

**Follow-up #15 (2026-07-22) — per-target dedup, fixing a performance cliff and an unbounded
brightness blowup near busy sites, both introduced by follow-up #14.**

User tested follow-up #14 and reported: performance tanks near target locations, and large white
balls appear in the cloud layer that glow intensely even at very low `beamGain`. Root cause of
both: follow-up #14's real march runs **once per satellite** (`beams[]`, up to `BEAM_MAX_ACTIVE` =
2048), but many satellites can share the same `bestIdx` ground target at once — Reflect Orbital's
"Disk" deployment (see follow-up #11's finding on constellation shape) means a popular site can
legitimately be serviced by dozens of satellites simultaneously. Two consequences of marching each
independently:
- **Performance**: near a busy target, many beams' cheap proximity pre-check pass *at once* for
  the same screen region (since they all cluster near the same ground point), so the expensive
  20-step real-density march — several texture fetches per step — runs once per contributing
  satellite instead of once per site. This is also why it was so much cheaper than the simple
  green debug rays: those samples are pure ALU (one `exp()` per sample, no texture reads at all),
  so their cost doesn't scale the same way with per-sample work; the real march's per-sample cost
  is dominated by `cloudDensity()`'s texture fetches, which the debug ray never pays.
- **Unbounded brightness**: `B_total +=` per beam sums every contributing satellite's glow
  independently at (nearly) the same screen location. Lowering `beamGain` scales each contributor
  down equally, but the total still scales with satellite *count* — for a large enough N, even a
  very low per-beam contribution multiplied by dozens of satellites reproduces the same blowup,
  which is exactly why turning `beamGain` down didn't fix the visual symptom.

**Fix: deduplicate by ground target, not by satellite**, before the expensive march ever runs.
Since `bestIdx` is already a bounded 0-200 index (one of the 201 fixed reflector targets),
`sat_orbit.comp` now ALSO writes a new small buffer — `ReflectTargetGlowBuf` (201 entries, one per
target) — using the exact same atomicMax-then-write-on-win arbitration `sat_flare.comp` already
uses for `sectorBright`/`flareEntries`: `atomicMax(targetBrightestBits[bestIdx],
floatBitsToUint(intensity))`, and the winning invocation writes its own `satENU`/`intensity`/
`targetENU`/`reflectDirENU`. The original per-satellite `ReflectBeamsBuf` (atomic-append, all
2048 slots) is untouched and still fully populated — it's still what the debug pointing-ray
visualization reads, since that legitimately wants to show every individual satellite converging.
`cloud_march.comp`'s real volumetric march now reads `ReflectTargetGlowBuf` instead, capping its
worst-case candidate count at 201 regardless of how many satellites are actually converging on any
one site — this is both the performance fix (independent of servicing satellite count) and the
brightness fix (exactly one bounded contribution per target instead of N stacked ones).

**Accepted approximation, disclosed:** "brightest wins" is not the same as "sum of all
contributors' irradiance" — physically, N mirrors really do deliver more total light to one spot
than one mirror. Summing would need atomic float addition (not natively available without a GPU
extension) or a fixed-point atomic-add workaround; picking the single brightest satellite per
target was the immediately-implementable fix using this codebase's existing, proven arbitration
idiom, and was judged the right tradeoff to actually land a fix now rather than block on a bigger
change. If it turns out visually important for a busy site's glow to visibly scale with how many
satellites are actually converging, summing is a real follow-up, not done here.

New buffer: `GpuReflectTargetGlows` (`SatelliteSim.h`) — `uint32_t brightestBits[201]` + 3 padding
words (std430 alignment before the vec3-containing struct array) + `GpuReflectBeam entries[201]`
(reuses the existing 48-byte per-entry struct as-is). New binding 5 on `sat_orbit.comp`'s
descriptor set (readwrite), binding 11 on `cloud_march.comp`'s (readonly) — three C++ edits
(`createOrbitDescriptors`, `createCloudMarchDescriptors`, buffer creation/cleanup) plus the
matching GLSL buffer declarations, following the same three-separate-descriptor-set-edits pattern
this file's plan section originally flagged for any new SSBO. Device-local (no CPU readback
needed, unlike `reflectBeamsBuf`'s host-visible mapping for its diagnostic). Zeroed each frame via
`vkCmdFillBuffer` alongside `reflectBeamsBuf`, same barrier structure (write in `sat_orbit.comp`
this frame, read by `cloud_march.comp` next frame — the same one-frame-in-flight pattern
`reflectBeamsBuf` itself already relies on).

Builds clean. **Not yet seen in-app.**

**Follow-up #16 (2026-07-22) — dedup was too effective: beams reduced to brief flickers.**

User tested follow-up #15 and reported beams were now nearly invisible, showing only brief
flickers instead of a steady glow. Root cause: the march's GEOMETRY (`satWorldPos`/`rayDir`) was
taken directly from `targetGlow[bi].satENU`/`.reflectDirENU` — i.e., from whichever satellite
currently *wins* the atomicMax brightness arbitration for that target. At a busy site, many
satellites converging on the same target have very similar intensity, so the winner can flip
between different physical satellites from one frame to the next. Each winner sits at a different
exact position with a slightly different reflection angle — and extrapolating that difference
~90km from satellite altitude down to the cloud shell amplifies even a fraction-of-a-degree
directional change into a shift of hundreds of meters to kilometers in the computed glow point,
easily enough to fall in and out of the pre-filter's ~21km proximity radius as the winner changes
— exactly "brief flickers, otherwise invisible."

**Fix: decouple brightness from geometry.** Brightness (picking a single winning satellite's
`intensity`) is fine to leave as-is — that's the accepted "brightest wins, not sum" approximation
from follow-up #15. But the march's geometric reference must stay stable regardless of *which*
satellite currently wins. `targetGlow[bi].targetENU` (the fixed ground target's position, which
only rotates slowly with Earth) already IS stable — so the fix anchors the march to the target's
own zenith line instead of the winning satellite's exact position/direction:
```
vec3 targetWorldPos = obsPos + targetGlow[bi].targetENU;
vec3 targetZenith   = normalize(targetWorldPos);
vec3 rayDir         = -targetZenith;
vec3 satWorldPos    = targetWorldPos + targetZenith * 200000.0; // safely above R_ATMOS
```
This matches the codebase's already-established "beams are well-approximated as vertical"
justification (see this C12 entry's shadow-map design section above, which used the same
reasoning to justify a sun-ward-only lookup) — only now applied to decide it's safe to *always*
treat the beam as coming straight down through the target's own zenith, rather than using each
individual satellite's actual (more accurate, but per-satellite-noisy once deduplicated) angle.
`satENU`/`reflectDirENU` remain in `GpuReflectTargetGlows`/read by this loop only implicitly (via
`intensity`, still the winner's) — the debug pointing-ray visualization is unaffected, since it
still reads the raw per-satellite `beams[]` list and uses each individual satellite's own true
`reflectDirENU` directly, which is correct there (no dedup, no winner-switching).

Builds clean. **Not yet seen in-app.**

**Follow-up #17 (2026-07-22) — full revert of #14-#16; replaced with a simple, non-cloud-aware
atmospheric-scattering glow, per explicit user direction.**

User feedback: "This consolidation of lights was a step in the wrong direction. Additionally,
nothing regarding the cloud lighting implementation has worked. Let's remove all of that." —
i.e., not just fix follow-up #16's flicker, but discard follow-ups #14 (real `cloudDensity()`
march), #15 (per-target dedup buffer), and #16 (stable-zenith geometry fix) entirely, and start
over from a much simpler baseline. Explicit instruction: **no cloud lighting yet.**

**Removed completely:**
- `GpuReflectTargetGlows`/`kReflectTargetCount` (`SatelliteSim.h`), `reflectTargetGlowBuf`/`Mem`
  members, its buffer creation/destroy/zero-fill/barriers, descriptor binding 5 on
  `sat_orbit.comp` and binding 11 on `cloud_march.comp` (both reverted to their pre-#15 binding
  counts), the atomicMax dedup write block in `sat_orbit.comp`, the `ReflectTargetGlowBuf` GLSL
  declaration and `beamCloudDensityAt()` function in `cloud_march.comp`.
- The entire real-march beam-glow block in `cloud_march.comp` (the `cloudDensity()`-based,
  shell-bounded, target-anchored version from #14/#15/#16).

**Replaced with** (still in `cloud_march.comp`, reads the raw per-satellite `beams[]` list
directly — no dedup, matching the user's "consolidation was wrong" verdict): a technique
structurally similar to the debug pointing rays just below it (multi-sample against the view ray
via the same safe fixed-point-projection trick, pure ALU, no texture reads) — but with brightness
driven by an analytic atmosphere model instead of a flat green constant, and non-uniform sampling
biased toward the ground:
- March range restricted to the atmosphere-entry-to-ground span only (`raySphere` against
  `R_ATMOS` then `R_EARTH`) — skips the vacuum portion of a beam's real length, same optimization
  rationale as before, just without any cloud-shell-specific narrowing.
- 32 samples distributed via an ease-out curve (`frac = 1 - (1-u)^3`) — consecutive samples are
  far apart near the atmosphere entry (nothing to see there) and close together near the ground
  ("added marches near the surface," per the user's request), fixing the debug ray's own
  uniform-spacing "dotted near the ground" artifact for this new effect (the debug ray itself is
  unchanged, still uniform).
- At each sample: local scattering-medium density via `exp(-h/H_R)` (Rayleigh) and `exp(-h/H_M)`
  (Mie) — this shader's own existing atmosphere constants, already driving the main sky/sun-
  transmittance model elsewhere in this file, reused here rather than inventing new ones. This
  directly implements the user's described physics: light entering the atmosphere is bright but
  there's no medium yet to scatter it into view (density ≈ 0 at high altitude); near the
  stratosphere a faint glow starts to appear (density rising); near the ground there's much more
  haze to scatter against, but by then the beam's own transmittance `T` has been reduced by
  everything already passed through, so visible brightness is `T × density` — a real
  single-scattering integral, not a hand-tuned altitude curve.
- `T` accumulates extinction incrementally per step (`T *= exp(-(betaRScalar*densR +
  BETA_M*1.1*densM)*stepLen)`, applied AFTER using `T` for that step's own contribution) — a
  scalar approximation (BETA_R's three channels averaged) since this pass has no color/tinting
  ambition yet, matching "start very simple."
- New `beamSkyGlowGain` (default 0.05, dim) is the primary user-facing brightness control — its
  own slider (Terrain tab, global idx 42), not overloading `beamGain` (the physical ground-
  irradiance term), per `[[feedback_shared_gain_sliders]]`. `hovCloudMinus`/`Plus`/`draggingCloud`/
  `cloudBufs` all resized 42→43 together, matching `[[feedback_cloud_slider_arrays]]`.
  `CloudMarchPC` grew 140→144 bytes for the new field, persisted in `settings.json` under
  `"beam_sky_glow_gain"`.

Debug pointing-ray visualization is completely unaffected — still reads the raw `beams[]` list
with each satellite's own true `reflectDirENU`, uniform 40-sample spacing, unchanged.

Builds clean. **Not yet seen in-app** — brightness scale (the `1e-9` constant), sample count, and
ground-bias curve shape are all first-pass guesses.

**Follow-up #18 (2026-07-22) — target elevation fix + shared ground/sky brightness control.**

User feedback (positive on #17's direction) plus two concrete requests: (a) tie the terrain
ground-spot visually to the new sky glow so it reads as "the ray starts here," and (b) fix ground
targets not accounting for real terrain elevation, which put every elevated target's "ground"
underground relative to the actually-rendered terrain.

- **Target elevation**: `reflectorTargetsECEF[]` were only ever unit ECEF directions, scaled by
  the constant `kEarthRadius` (sea level) when converted to a real ECI position in
  `updatePositions()`. Fixed by adding `reflectorTargetsRadiusM[kNumReflectorTargets]`
  (`SatelliteSim.h`), computed once in `buildOrbits()`'s target-generation loop using the SAME
  CPU-side `earthElevCpu` lookup/formula the observer's own terrain height already uses (`init()`
  order guarantees `createGlowResources()` — which loads `earthElevCpu` — runs before
  `initConstellation()`/`buildOrbits()`). `updatePositions()`'s ECI conversion now multiplies by
  `reflectorTargetsRadiusM[ti]` instead of the bare `kEarthRadius`. This one CPU-side fix
  propagates everywhere a target's ground position is used — `sat_orbit.comp`'s `targetENU`,
  `sat_sky.frag`'s ground-spot, and follow-up #19's new upward march below — without touching any
  of those consumers directly.
- **Discovered, NOT fixed (flagged for the user, out of this round's scope):** target indices `0`
  and `kNumReflectorTargets-1` (200) are never actually assigned a direction by the generation loop
  (`for (ti=1; ti<kNumReflectorTargets-1; ...)` — deliberately or accidentally skips both ends).
  Both stay at their zero-initialized `vec3(0,0,0)`, which `normalize()`s to NaN, making
  `sunDot` NaN and `valid` always false — i.e., these two slots are permanently inert. Comments
  in this same function still describe index 200 as "the fixed target at the observer spawn point
  (67°S, 67°W)," matching this session's memory notes about a duplicate-assignment bug fixed
  2026-07-20 — it looks like that fix removed the erroneous line but the CORRECT one never existed
  in the version examined this session, or was removed alongside it. Not touched this round (out
  of scope for elevation/march-direction/ground-spot); worth a dedicated look next session.
- **Shared brightness control**: `beamSkyGlowGain` added to `SatDrawPC` (`sat_sky.frag`'s push
  constant, mirroring `CloudMarchPC`'s copy — grew 164→168 bytes) and used in the ground-spot term
  as `skyGlowNorm = pc.beamSkyGlowGain / 0.05` (normalized against the slider's default so existing
  footprint brightness is unchanged unless the user actually moves the slider) — multiplying the
  ground spot's existing `kBeamGroundScale`-based brightness. Also added a small, bright "hotspot
  core" (`footprintR * 0.15` radius Gaussian, added on top of the existing soft halo) so the
  ground-spot reads more like a distinct landing point — the anchor the new upward sky march
  (follow-up #19) visually starts from.

Builds clean. **Not yet seen in-app.**

**Follow-up #19 (2026-07-22) — reversed march direction + altitude-bounded range.**

User's specific ask: marching the whole ~100km atmosphere per beam is expensive; since the
volumetric effect only matters in the lower atmosphere, march UP from the (now elevation-correct)
ground target toward the satellite instead of DOWN from the satellite, and stop relatively quickly.

- New origin: `targetWorldPos = obsPos + beams[bi].targetENU` (elevation-correct per follow-up
  #18) instead of `satWorldPos`. Direction: `rayDirUp = -beams[bi].reflectDirENU` (that field
  points sat→ground, the direction light propagates; negated for the upward march).
- March range bounded to `kBeamGlowMaxAltM = 25000.0` (25km) above sea level via
  `raySphere(targetWorldPos, rayDirUp, R_EARTH + kBeamGlowMaxAltM)` — the far root is the march's
  new "top." This alone shrinks the marched span from ~100km to ~25km (roughly a 4x reduction at
  the same sample count/resolution) — the direct fix for "marching through the entire atmosphere
  is probably very expensive."
- **Transmittance direction, the one subtlety this reversal introduces:** `T` must still represent
  "how much of the ORIGINAL light survived from space down to here," which is a property of the
  full path regardless of which way the shader marches computationally. Naively accumulating T
  from ground (T=1) upward would be backwards (it would mean "nothing extinguishes below this
  point," the opposite of reality). Fixed by iterating samples from the TOP of the truncated range
  down to the ground (T=1 at the top, decreasing as altitude drops) — physically correct in a
  single pass, and a reasonable approximation given the top of this truncated range (25km) is
  already high enough that H_R's ~8km scale height leaves very little unaccounted-for extinction
  above it. The existing ease-out density curve (`frac = 1-(1-u)^3`) is reused as-is, just applied
  as `t = tTop*(1-frac)` instead of the old `tStart + frac*span` — still dense near the ground,
  sparse near the top, same curve shape, opposite endpoint mapping.
- Debug pointing-ray visualization is unaffected (still sat→ground, full uniform-sampled length) —
  not touched this round; its own "dotted near the ground" artifact (the original trigger for
  follow-up #14, back before the two reverts) still isn't separately fixed.

Builds clean. **Not yet seen in-app** — `kBeamGlowMaxAltM`'s exact value (25km) is a first-pass
guess at "relatively quickly," may need retuning once seen.

**Follow-up #20 (2026-07-22) — four fixes: lost/incoherent rays, downsampling for perf, ocean
occlusion bug, and slow observer-move catch-up.**

User feedback after follow-up #19 landed, four distinct issues:

1. **"Lost rays" not pointing at anything in particular.** Diagnosed and confirmed NOT the
   zero-vector targets (indices 0/200 — those are correctly filtered out: `normalize(vec3(0))` is
   NaN, `sunDot` becomes NaN, `valid = (NaN < 0.0)` is false in IEEE754, so `td.w < 0.5` skips them
   in `sat_orbit.comp`'s bestIdx scan — they're inert, not selectable, not the cause). Real cause:
   follow-up #19's march uses `beams[bi].reflectDirENU` (the mirror's ACTUAL current orientation)
   for the march DIRECTION, starting at the fixed TARGET position. `reflectDirENU` lags the real
   target for several seconds after a retarget while `MIRROR_ROT_RATE`-limited slewing catches up
   — during that window it points somewhere between the old and new ideal direction, and since the
   march no longer starts AT the satellite (follow-up #19 changed the origin to the target), a
   mid-slew direction produces a ray that doesn't visually connect to anything sensible. Fixed by
   using a STABLE vertical approximation instead: `rayDirUp = normalize(targetWorldPos)` (straight
   up through the target's own zenith), not `reflectDirENU`. Well-justified because `bestIdx`
   selection already maximizes `dot(satZenith, targetDir)` — targets are always close to the
   servicing satellite's nadir, so "straight up" is a good approximation regardless of slew state.
   `intensity` still legitimately depends on the satellite's real current orientation (via
   `cosIncidence`, upstream in `sat_orbit.comp`) — only the glow's GEOMETRY stopped depending on it.
2. **Performance still rough at busy sites.** Added stable downsampling: `GpuReflectBeam.debugPad`
   (previously pure std430 padding, unused) is repurposed to carry the originating satellite's own
   stable dispatch index (`float(i)`, written in `sat_orbit.comp`). `cloud_march.comp`'s sky glow
   now only marches every 8th satellite (`mod(debugPad, 8.0) == 0`, `kBeamGlowStride = 8.0`),
   compensating with `intensity *= 8.0` so total apparent brightness at a busy site stays close to
   before with an 8x reduction in actual marches. Striding on the atomic-append SLOT index (`bi`)
   directly was considered and rejected — `bi` isn't stable frame-to-frame (races on `beamCount`),
   so it would show a different random 1/8th of satellites each frame (flicker); striding on the
   satellite's own stable index avoids that.
3. **Rays don't draw over ocean.** Root cause: `cloud_march.comp`'s `tEnterCombined` (used by
   `sat_sky.frag`'s terrain/ocean occlusion test — "if a surface hit is closer than this, hide the
   whole cloud/sky composite") was computed ONLY from the cirrus/cloud shell's own entry distances,
   with no knowledge of where the beam glow itself actually starts (very close to the ground, per
   follow-up #19). Whenever a real surface hit was closer than a FAR cloud shell but FARTHER than
   the NEAR beam glow, the whole composite — including the beam glow, since it shares the same
   target image — was wrongly suppressed. This hit ocean far more often than terrain specifically
   because the ocean's smooth fallback sphere produces a valid `tSurface` hit in nearly every view
   direction, while terrain-less directions (looking at open sky away from any landmass) often have
   no hit at all, letting the false-suppression condition trigger far more rarely there. Fixed by
   tracking `tEnterBeam` (the nearest VIEW-RAY distance any beam-glow sample actually contributed
   at, using the same `tRay` already computed for the proximity check) and folding it into
   `tEnterCombined`'s final `min()` — the occlusion test now knows about the near-ground beam
   content too, not just the cloud shell.
4. **Noticeable catch-up delay when the observer moves.** Confirmed as a real, deliberate rate
   limit — `MIRROR_ROT_RATE` was a hardcoded 1 degree/second in `sat_orbit.comp`, reasonable for
   passive observation but slow (potentially minutes) for a satellite to re-slew toward a newly
   relevant target after the observer teleports/moves to a new area. Made tunable:
   `SatOrbitPC.mirrorSlewDegPerSec` (new field, `SatOrbitPC` grew 104→108 bytes), CPU member
   `SatelliteSim::mirrorSlewDegPerSec` defaulting to 15 (15x faster than before), new settings
   slider ("Mirror slew rate (deg/s)", 1-60 range, global idx 43 — `hovCloudMinus/Plus/`
   `draggingCloud`/`cloudBufs` resized 43→44), persisted in `settings.json`. `sat_orbit.comp` now
   computes `maxAngle = radians(pc.mirrorSlewDegPerSec) * pc.simDt` instead of a fixed constant.

Builds clean. **Not yet seen in-app.**

**Follow-up #21 (2026-07-22) — direction fix (satellites again), slew-rate revert, and a diagnosed
(not yet fixed) observer-movement lag.**

User feedback after follow-up #20: performance improved marginally, but beams "do not point
towards satellites at all" and now emit straight up from the target regardless — follow-up #20's
"stable vertical" fix over-corrected. Also asked to revert the mirror slew-rate default, and asked
why target sites/rays lag behind terrain when the observer moves.

- **Direction fixed properly:** `rayDirUp = normalize(beams[bi].satENU - beams[bi].targetENU)` —
  points at the satellite's ACTUAL current position, not a fixed vertical approximation, and not
  `reflectDirENU` (which, on reflection, was never even the geometrically correct vector for
  "target→satellite" — it's the direction reflected SUNLIGHT travels, which only coincides with
  that line if the mirror happens to be perfectly converged). Using the two real positions
  directly is correct regardless of mirror slew state (no dependency on `surfN0`/`reflectDirENU`
  at all) and always points at where the satellite really is. Computed as `satENU - targetENU`
  rather than `satWorldPos - targetWorldPos` to avoid re-adding then subtracting the shared
  `obsPos` term.
- **Mirror slew rate default reverted** to 1 deg/sec (was raised to 15 in follow-up #20) per user
  preference. Left as a tunable slider (in case wanted later) rather than removing it outright.
- **Observer-movement lag — diagnosed, not yet fixed.** Root cause: `cloud_march.comp`'s dispatch
  runs BEFORE `sat_orbit.comp`'s dispatch within the same `recordCompute()` call (an existing,
  intentional ordering — cloud_march.comp must finish before the render pass begins, and this
  block was written early in the function before the orbit/flare dispatches were added later).
  This means whenever `cloud_march.comp` reads `ReflectBeamsBuf` (for the sky glow), it's reading
  data `sat_orbit.comp` wrote during the PREVIOUS frame's `recordCompute()` call — one frame stale
  by construction (the same "previous frame's data is safe to read" pattern this codebase uses
  elsewhere, e.g. `glowBuf`). For most buffers that staleness is harmless (position/observer-
  independent data). For beams specifically it isn't: `satENU`/`targetENU` are stored as METERS
  offsets in the OBSERVER'S ENU basis AT WRITE TIME — if the observer moves between frames, that
  stored offset no longer matches the CURRENT frame's observer position/basis when
  `cloud_march.comp` adds it onto this frame's fresh `obsPos`. The resulting position error is
  approximately equal to the observer's own displacement (translation) plus a smaller rotational
  term from the ENU basis itself reorienting, both scaling with how far the observer moved in that
  one frame — imperceptible at typical walking speed, clearly visible at "boost" movement speed
  (which can cover tens of km in a single frame).
  - **Why not fixed immediately this round:** the only fully correct fix is for `cloud_march.comp`
    to read the SAME frame's fresh beam data, i.e., reorder its dispatch to after
    `sat_orbit.comp`'s. This is NOT purely mechanical: `cloud_march.comp`'s dispatch currently sits
    BEFORE the `if (activeSatCount == 0) return;` early-out (deliberately, so clouds keep rendering
    with zero active satellites) — moving it after `sat_orbit.comp` requires also moving
    `sat_orbit.comp` (with 0 workgroups when `activeSatCount==0`, itself a legal/cheap no-op) ahead
    of that same early-out, while keeping `sat_flare.comp` gated by it. It also touches the fixed
    GPU-timestamp slot assignments (session 29 profiling — slots 1-4 have specific meanings the
    "GPU FRAME BREAKDOWN" UI and `updateGpuTimingStats()` depend on) which would need renumbering
    to stay meaningful. A translation-only "correct for how far the observer moved since last
    frame" patch was considered as a lower-risk alternative but rejected: it doesn't correct the
    ENU-basis-rotation component, which is NOT negligible for the satellite offset specifically
    (up to ~2000km magnitude — even a fraction-of-a-degree basis rotation error amplifies to
    multiple km) — i.e., it wouldn't fully fix the exact case (fast/boost movement) the user
    reported as most noticeable. Flagged for the user rather than either rushing the full reorder
    or shipping a partial fix with a known residual error in the reported worst case.

Builds clean. **Not yet seen in-app** (direction fix, slew revert). Lag issue awaiting a decision
on whether to do the full dispatch-reorder fix.

**Follow-up #22 (2026-07-22) — full dispatch reorder to fix the observer-movement lag.**

User chose the full reorder over a partial translation-only patch (which wouldn't have corrected
the ENU-basis-rotation component of the error). Implemented in `recordCompute()`
(`SatelliteSim.cpp`):

- `sat_orbit.comp`'s entire dispatch (enabledMask/highlightMask computation, `SatOrbitPC` fill,
  `reflectBeamsBuf` zero-fill + barrier, the dispatch itself, both post-dispatch barriers) moved to
  run BEFORE `cloud_march.comp`'s dispatch — previously it ran after `cloud_march.comp` AND
  `cloud_shadow.comp`, alongside `sat_flare.comp`. It now runs unconditionally, including when
  `activeSatCount == 0`: `vkCmdDispatch(cmd, (0+63)/64, 1, 1)` dispatches 0 workgroups, which is a
  legal Vulkan no-op (any dispatch dimension being zero means no invocations occur) — cheap and
  safe. This was necessary because `cloud_march.comp` (and therefore its beam sky glow) must keep
  rendering even with zero active satellites, and previously sat_orbit.comp's dispatch sat AFTER
  the `if (activeSatCount==0) return;` early-out specifically to avoid dispatching orbital
  mechanics work for zero satellites — moving it ahead of that check (as a cheap no-op in the
  empty case) resolves the conflict.
- The `if (activeSatCount==0) return;` early-out now only needs to guard `sat_flare.comp` (moved to
  after `sat_orbit.comp`/`cloud_march.comp`/`cloud_shadow.comp`, all of which now always run) — it
  went from writing two placeholder timestamps (orbit + flare) to just one (flare).
- **GPU timestamp slots renumbered to match the new execution order** (numbers 1-4 unchanged,
  reassigned to different work): 1 = orbit compute (was 3), 2 = cloud march (was 1), 3 = cloud
  shadow map (was 2), 4 = flare compute (unchanged). Updated in three places to stay consistent:
  `updateGpuTimingStats()`'s `raw[]` comments, `kPerfLabels[]` in `SatelliteSimUI.cpp` (the "GPU
  FRAME BREAKDOWN" settings panel), and `savePerfSnapshot()`'s JSON key/index mapping (key NAMES
  are what matter for reading old snapshot files — a re-ordering like this only changes which
  index a given name reads from going forward, doesn't invalidate old data).
- Net effect: `cloud_march.comp`'s Reflect-Orbital beam sky glow now reads `ReflectBeamsBuf` as
  written by `sat_orbit.comp` earlier in the SAME frame — same-frame fresh data, no staleness, so
  beam positions track the observer's current-frame position exactly like terrain does, eliminating
  the lag entirely (not just reducing it) regardless of how fast the observer moves.

Builds clean (verified no duplicate variable declarations from the block move — `orbitPc`/
`enabledMask`/`highlightMask` each appear exactly once). **Not yet seen in-app.**

**Follow-up #23 (2026-07-22) — elevation-lookup precision + analytic closest-approach rewrite
(replacing the discrete 32-sample march). Note: this follow-up was implemented and build-verified
in a session that lost context before it could be written up here — documented after the fact,
alongside follow-up #24 which fixed a real bug introduced by it.**

User reported four issues against follow-up #22's state: (1) beams still visibly converge to a
point below the rendered terrain surface on elevated ground, and the underground portion isn't
occluded — visible "through" the hillside; (2) beams draw over atmosphere/cloud layers with no
depth/extinction awareness, and asked for smarter start/end culling against cloud volumes; (3)
when the observer moves directly over a target, all volumetric effects glitch/alias/flicker
together; (4) beams viewed near-horizontally show visible dotted stepping from the discrete march,
and asked specifically: why does the march produce flat circular dots, and would a signed distance
field help?

- **Elevation-lookup precision (issue 1, partial fix):** `earthElevCpu` is itself a 10x
  point-sampled downsample of the real 21600x10800 elevation texture (see `createGlowResources()`)
  — a single lookup for a target's exact lat/lon could land on a texel up to ~9km away from the
  true position, missing a nearby peak and underestimating height, which is the direct cause of
  "converges below the surface." Fixed by taking the MAX over the surrounding 3x3 texels (wrapping
  longitude) instead of a single point sample — can only raise the estimate toward a real nearby
  peak, never lower it — plus a fixed +75m safety margin, biasing toward "floats slightly above
  ground" rather than "sinks into it," since the former reads far better. Does not address the
  "beams through terrain, unoccluded" half of issue 1 — that requires per-sample terrain-depth
  awareness in a currently terrain-blind compute pass (`cloud_march.comp` has no elevation texture
  binding), a bigger architectural addition not attempted this round.
- **Analytic closest-approach + closed-form extinction, replacing the discrete march (issues 2 and
  4):** the beam's density model is a pure function of altitude (`exp(-h/H)`), and altitude varies
  near-linearly along the beam over this short a march — so both "closest approach to the view
  ray" and "accumulated extinction to that point" have closed-form solutions, eliminating discrete
  sampling (and its dotting artifact) entirely:
  - Ray-to-line-segment closest point: standard two-line closest-point solve between the view ray
    (`obsPos + s*dir`) and the beam segment (`targetWorldPos + t*rayDirUp`, `t` in `[0,tTop]`),
    with a near-parallel fallback (project `r` directly onto the beam direction) for when the
    denominator vanishes.
  - Closed-form optical depth: `∫exp(-(h0+mu*t)/H)dt = (H/mu)*(rho(0)-rho(t))` for `mu != 0`
    (falls back to `rho0*t` as `mu->0`), using `BETA_R`/`BETA_M`/`H_R`/`H_M` — this shader's own
    existing atmosphere constants, unchanged from follow-up #17.
  - `tEnterBeam` (terrain/ocean occlusion, follow-up #20) becomes a single analytic `sView` value
    instead of a running minimum over 32 samples.
  - Not attempted: issue 2's "cloud-volume-aware start/end culling" — that needs real
    `cloudDensity()` sampling along the beam, explicitly out of scope per the standing "no cloud
    lighting for beams" instruction from the #14-#17 revert.
  - Leading theory for issue 3 (zenith flicker): discrete per-sample evaluation is inherently
    unstable at near-parallel viewing angles (observer looking straight down a near-vertical beam)
    — tiny shifts in exactly which of 32 fixed samples projects nearest a pixel's view ray can flip
    frame to frame. An analytic, continuous closest-point evaluation should be far more stable
    here by construction, though this was not yet confirmed in-app before context was lost.

Builds clean. **Not seen in-app before the two follow-ups below were needed.**

**Follow-up #24 (2026-07-23) — critical closest-point formula bug fix + grazing-angle brightness +
smooth capsule end-fade.**

Testing follow-up #23 surfaced two new problems: "beam rendering is now highly observer dependent
— ruins the effect of seeing beams converge. Beams are clipped, hard edges sometimes — I feel this
isn't just a realism thing" and "beams only manifest from a distance as a couple of dots at the
cloud layer above target sites... everything below the cloud layer is stopped no matter cloud
cover amount."

- **Root cause, found by re-deriving the closest-point math from scratch:** minimizing
  `|r + s*dir - t*rayDirUp|^2` gives `s = t*bDot - c` and `t = f + s*bDot`
  (`bDot=dir.rayDirUp`, `c=dir.r`, `f=rayDirUp.r`); substituting and solving gives
  `t = (f - bDot*c) / denom`. Follow-up #23 had `(bDot*f - c) / denom` — `bDot` multiplying the
  WRONG term. This produced systematically incorrect (and frequently out-of-segment, hence
  clamped) closest points for any non-perpendicular viewing angle — directly explaining both
  reports: wrong/unstable solutions jumping between the true and clamped answer as view angle
  changed ("hard edges... observer dependent"), and oblique/grazing distant views — exactly where
  a wrong `bDot` term diverges most from correct — clamping to the wrong segment endpoint for most
  of the screen instead of tracing a continuous line to the ground ("couple of dots... everything
  below is stopped," which was NOT cloud occlusion — no cloud-related code path is even involved
  in this term at all). Fixed: swapped to the correct `(f - bDot*c)/denom`.
- **Grazing-angle brightness (restores "convergence looks bright," properly this time):** the old
  discrete 32-sample march accidentally over-brightened near-end-on views (many overlapping
  samples all close to the same near-parallel view ray, summed) — part of why distant "many beams
  converging" shots looked good, and exactly what follow-up #23's single-point-per-beam evaluation
  lost, with no accidental bonus left standing in for it. Replaced with the actual analytic
  integral of a Gaussian cross-section tube crossed by a straight line at angle theta: distance
  from a point on the view ray to the beam's line grows as `sqrt(perpDist0^2 + (Δs*sinTheta)^2)`
  moving away from the closest-approach point, so
  `∫exp(-dist^2/(2σ^2))dΔs = proximity0 * σ*sqrt(2π)/sinTheta` — i.e. multiply the existing
  point-evaluated brightness by `kBeamRayRadiusM/sinTheta`, capped at `tTop` (a real, finite
  segment can't integrate to more "effective length" than its own length, so `sinTheta` is
  clamped to at least `kBeamRayRadiusM/tTop` before dividing) to stay bounded as `sinTheta->0`
  (the exact "observer directly overhead" case).
- **Smooth capsule-style end fade:** the hard `clamp(tBeamRaw, 0, tTop)` from follow-up #23 pinned
  many different view rays to the SAME fixed endpoint at full brightness whenever their true
  (unclamped) closest point fell outside the segment — a real "flat disc" artifact at each end, not
  a stylistic rough edge. Fixed with `endFade = exp(-(overshoot^2)/(2*kBeamRayRadiusM^2))` where
  `overshoot` is how far the UNCLAMPED `tBeamRaw` falls beyond either end, using the same Gaussian
  sigma as the tube radius — reads as a rounded capsule cap instead of a flat one.
- Old flat `kBeamRayRadiusM` brightness stand-in (follow-up #23's placeholder "effective length,"
  since there was no discrete step to weight by anymore) removed — `grazingLen` and `endFade` now
  do that job properly.

Builds clean. **Not yet seen in-app.**

**Follow-up #25 (2026-07-23) — aurora terrain leak, beam pop-in, atmospheric color.**

User confirmed follow-up #24 "definitely better at a distance and performance-wise," then reported
three more issues, diagnosed via three parallel Explore-agent investigations plus direct reads:

- **Aurora draws through terrain at 38km — NOT caused by the dispatch reorder or beam work,** despite
  the timing coincidence; a separate, pre-existing gap. `tEnterCombined` (what `sat_sky.frag`'s
  terrain gate checks) was only ever built from `tEnterCirrus`/`tEnterCloud`/`tEnterBeam` —
  `auroraMarchCS`'s own entry distance (`atEnter`, already computed locally) was never returned or
  folded in, even though aurora's radiance goes straight into the same `B_total` that gate protects.
  Whenever cirrus/cloud/beam are all absent on a ray — the COMMON case above the cloud shell, e.g.
  ~38km, where `cloudMarchCS`'s "above shell" branch frequently early-returns — `tEnterCombined`
  stayed -1, the gate's `>= 0.0` check failed open, and aurora composited unconditionally regardless
  of `tSurface`. Fixed: gave `auroraMarchCS` an `out float tEnterOut` (same convention as
  `cloudMarchCS`/`cirrusMarchCS` — set once the march genuinely runs, before accumulation), folded
  into `tEnterCombined` with the same `min`-combine pattern already used for `tEnterBeam`.
- **Beam pop-in:** root cause was a hard `if (length(targetENU) > pc.beamMaxRangeM) continue;` —
  a pure step function, no fade region — compounded by the 8x stride-downsample compensation
  (`kBeamGlowStride`), so a stride-eligible beam snapped straight to full 8x brightness the instant
  its target crossed into range. Fixed: replaced the hard cutoff with `rangeFade = 1.0 -
  smoothstep(beamMaxRangeM - kBeamRangeFadeM, beamMaxRangeM, dist)` (fade constant 50km),
  multiplied into the final per-beam contribution instead of gating it outright (still `continue`s
  once truly zero, as a cheap perf floor).
- **White blobs, orientation-dependent clipping/z-fight when standing in a beam** — three fixes,
  all in the same block:
  - **Color:** confirmed at the code level (not just perception) that the emission term was a
    literal `vec3(scalar)` — `haze = rhoR + rhoM` with a mono-averaged `betaRScalar` transmittance,
    genuinely colorless by construction. This file already duplicates real Rayleigh/Mie machinery
    for a different purpose (`skyAmbientBase`, ~line 830: integrate exponential density, phase-
    weight, scale by vector `BETA_R`) — ported that pattern in: transmittance `T` now uses vector
    `BETA_R` (wavelength-dependent extinction), and the emission is now
    `phaseR(cosA)*BETA_R*rhoR + phaseM(cosA)*BETA_M*rhoM` where `cosA` is the beam-to-sun angle —
    the beam glows because it's sunlit air, the same physical mechanism as the rest of the sky.
  - **Orientation-dependent clipping:** traced to the hard `if (denom > 1e-5)` branch in the
    closest-point solve — `denom = sin²(angle between view ray and beam)`, so that branch boundary
    is a literal thin ring in view-direction space around "looking straight along this beam."
    With several differently-angled beams in view at once (a busy multi-mirror target, some near-
    horizon), several such rings can land anywhere on screen including the edges, and dividing by a
    shrinking-but-not-yet-clamped `denom` amplifies float noise right at the boundary. Fixed by
    blending the general and near-parallel formulas continuously (`mix(tBeamParallel, tBeamGeneral,
    smoothstep(0.0, 3e-4, denom))`) instead of hard-switching — no more branch, no more seam.
  - **Many-beams overwhelm:** every beam added unboundedly into `B_total` (further inflated by the
    8x stride compensation), so a dense cluster could blow well past displayable range and read as
    flat clipped white. Fixed by accumulating this block's beams into a local `beamRadiance` and
    applying a soft saturating compression once (`B_total += vec3(1.0) - exp(-beamRadiance)`)
    instead of adding raw — bounds the worst case, preserves the new color better than a hard clamp,
    doesn't touch the stride/downsampling scheme itself (not a reported perf problem).

Builds clean. **Not yet seen in-app.**

**Follow-up #26 (2026-07-23) — aurora fix incomplete + beam glow went fully invisible.**

Testing follow-up #25 surfaced: (1) aurora still popped in over terrain at ~38-40km, now described
more precisely as "immediately pops in over terrain, then fades rapidly over the next few km
descending" (previously just "draws through terrain"); (2) beams became completely invisible even
at max `beamGain`/`beamSkyGlowGain` — only the ground hotspots remained visible.

- **Aurora — #25's fix was necessary but incomplete.** `40000 = kAuroraShellInnerM(95000) -
  55000` — the reported pop altitude is almost exactly `kAuroraMarchInnerM`, confirming the real
  bug: `auroraMarchCS`'s march bounds (`kAuroraMarchInnerM/OuterM`) are deliberately padded 55/110km
  beyond the REAL emissive shell (`kAuroraShellInnerM/OuterM`) to catch the falloff/oval-mask tail
  smoothly — so "inside the padded march range" (branch 2, `atEnter=0`) does NOT mean "inside the
  real emissive shell." #25's fix set `tEnterOut = max(atEnter, 0.0)`, which is 0 across the ENTIRE
  padding band (~40-95km altitude here) — telling the terrain gate "content starts at the camera,
  nothing can ever occlude it" for that whole band, even though real brightness only starts near
  the true 95km shell. That's the hard on/off "pop over terrain" at the padding boundary, and the
  "fades rapidly over the next few km" is the genuinely-correct-but-newly-exposed extinction/oval
  falloff becoming visible once the pop stopped masking it. Fixed properly: compute `tEnterOut`
  against the REAL (unpadded) `kAuroraShellInnerM/OuterM` bounds via a small separate `raySphere`
  classification, fully decoupled from the padded `atEnter`/`atExit` still used for the actual step
  march below (unchanged — the padding still does its job for the visual falloff itself, it was
  only wrong to reuse for the terrain-occlusion distance).
- **Beams invisible — a real magnitude bug in #25's color fix, not a slider/tuning issue.** The new
  `hazeColor = pR*BETA_R*rhoR + pM*BETA_M*rhoM` used `BETA_R`/`BETA_M` (~1e-5, real physical
  scattering coefficients) as an ABSOLUTE multiplier, where the old flat `haze = rhoR+rhoM` was
  O(1) — a ~4-5 order-of-magnitude collapse that `kBeamScale=1e-9`/`beamSkyGlowGain` were never
  calibrated for, explaining "cannot see beams anymore, even with very high beam gain and beam sky
  glow settings" (no slider has that much range). Fixed by decoupling HUE from MAGNITUDE: derive
  the color ratio from the Rayleigh/Mie phase-weighted `BETA` terms (so it's correctly blue-tinted,
  not white), then rescale the result to the SAME total magnitude as the old scalar
  (`hazeMagnitude = rhoR+rhoM`) via `hazeColor = (rayleighWeighted + mieWeighted) *
  (hazeMagnitude / colorMagnitude)`. This keeps the existing `kBeamScale`/`beamSkyGlowGain`
  calibration valid — no guessed magic recalibration constant needed — while still fixing the
  original "white blob" complaint via the ratio.
- **Lesson:** introducing real physical units (`BETA_R`/`BETA_M`, ~1e-5) into a term that was
  previously a normalized/flat scalar (~O(1)) and is fed into an already-tuned scale constant is a
  silent, easy-to-miss magnitude bug — the code still compiles and runs, just produces a
  vanishingly small (or, in the aurora case, a wrongly-always-zero-distance) result. When porting a
  "real" physical formula into a place calibrated against a stylized stand-in, either recalibrate
  the scale constant deliberately (with the math shown, not guessed) or — preferable when
  possible — decouple the physically-meaningful part (hue, entry classification) from the
  previously-tuned part (magnitude, fallback distance) so existing calibration survives unchanged.

Builds clean. **Not yet seen in-app.**

**Follow-up #27 (2026-07-23) — beam color: fixed transmittance direction, reverted hue-mixing.**

User confirmed follow-up #26 "fixed the beam visuals" (beams visible again), but reported the new
color reads as "mesmerizing rainbows" rather than physically plausible — and correctly reasoned
that a beam is just reflected sunlight, so it should look white/yellow/red (like a sunbeam or
low-angle sunlight), not blue (like ambient Rayleigh-scattered sky).

- **Root cause of the rainbow instability:** follow-up #25/#26's color derived HUE from a ratio
  between `pRBeam*BETA_R*rhoR` (a blue-dominant vector) and `pMBeam*BETA_M*rhoM` (a near-neutral
  scalar). Two things made this ratio unstable rather than a smooth gradient: (1) `H_M = 12.0`
  (meters!) means `rhoM` collapses to ~0 within meters of the ground, so the Rayleigh:Mie ratio
  swings enormously over a tiny altitude range near the base of every beam; (2) `pMBeam` (Henyey-
  Greenstein, `G_MIE=0.26`) swings hugely with viewing/sun angle. Together: rapidly, chaotically
  shifting hue point-to-point and beam-to-beam — "rainbows."
- **Root cause of the wrong hue direction (blue instead of white/yellow/red):** even setting
  instability aside, modeling the beam's color as Rayleigh/Mie SCATTERING hue was modeling the
  wrong physical effect. The beam carries direct sunlight reflected in space (unfiltered by
  Earth's atmosphere until it starts descending) — its white→yellow→red gradient should come from
  wavelength-dependent EXTINCTION accumulating along its remaining descent (blue attenuates faster
  than red — the same reason a low sun looks orange), not from a scattering-angle-dependent hue at
  the glow point itself. That mechanism already existed (`T`, the vector transmittance) but was
  measuring optical depth in the WRONG direction: `ODR = (H_R/mu)*(rho0R-rhoR)` integrates from the
  GROUND (t=0) up to the evaluation point — meaning attenuation was LARGEST near the satellite end
  and near-zero (white) near the ground, backwards from a real sunbeam. Harmless while `T` was
  colorless (follow-ups #23/24), but wrong now that color depends on it.
- **Fix:** (1) flipped `ODR`/`ODM` to integrate from the evaluation point UP to the segment's fixed
  top (`kBeamGlowMaxAltM`) instead of from the ground: `rhoTopR/rhoTopM` (density at that fixed
  altitude — a constant) replace `rho0R/rho0M`, giving `ODR = (H_R/mu)*(rhoR - rhoTopR)` — zero
  near the satellite end (nothing traversed yet), maximal near the ground (most atmosphere already
  fallen through) — the correct direction. `T` (already using vector `BETA_R`, unchanged from #25)
  now naturally reddens toward the ground and stays near-white near the top. (2) Reverted the
  emission color to the flat/neutral `hazeColor = vec3(rhoR + rhoM)` (same as before #25's hue
  attempt) — a collimated beam scattered sideways by local air/haze reads as close to neutral at
  the scattering point; the white→yellow→red gradient now comes entirely from the (now correctly
  directed) `T`.
- **Lesson:** when a "physically real" term is plugged into a formula, check not just its
  MAGNITUDE (follow-up #26's lesson) but its DIRECTION/SIGN CONVENTION against the actual physical
  scenario — `ODR` computed the right integral in the wrong direction, which stayed invisible while
  unused for color and only became a visible bug once the vector `T` started driving hue.

Builds clean. **Not yet seen in-app.** Issues 2 (terrain/cloud occlusion, daytime suppression) and
3 (convergence-point stability) from this round are substantial new scope — not diagnosed bugs in
existing code but previously-deferred capability ("Not attempted... a bigger architecture
addition") — see the next planning entry once designed.

**Follow-up #28 (2026-07-23) — terrain/cloud occlusion, daytime suppression, convergence-point
stability. Planned (two Explore-agent research passes) before implementing, given the scope.**

- **Per-beam terrain occlusion:** the shared `tEnterCombined` gate is a single per-pixel scalar
  (nearest volumetric content on that ray) — structurally can't encode occlusion for multiple
  depth-separated beams on the same ray; whichever beam is nearest the camera masks the occlusion
  test for every other beam sharing that ray, including ones geometrically behind a mountain. Fixed
  by giving `cloud_march.comp` its own `earthElevTex`/`earthSpecTex` bindings (11/12 — mirrors the
  exact `lightDomeBuf`/`auroraNoiseTex` precedent for adding a binding to this descriptor set, same
  elevation decode `sat_sky.frag` uses) and doing a per-beam check directly in the loop:
  `beamTerrainOccluded()` marches 8 samples along the straight 3D line from the observer to the
  beam's own closest-approach point `p`, checking terrain height at each — geometrically exact for
  "does a hill block this specific line of sight" (unlike the shared scalar), since line-of-sight
  really is a straight line in this file's coordinate frame. A hard boolean skip, not a fade — a
  hill blocking a sightline is a genuine hard edge in reality, not an artifact.
- **Cloud occlusion:** confirmed in code that beams previously had ZERO interaction with cloud
  opacity — `B_total = B_cirrus*A_cloud + B_cloud` (cirrus IS attenuated by cloud opacity) but the
  beam term was `+=`'d with no multiply at all. Fixed: `B_total += (vec3(1.0)-exp(-beamRadiance)) *
  A_total`. Same "shared per-ray opacity, no depth-awareness" approximation already accepted for
  cirrus/cloud, well-justified here since beams render up to 25km — above typical cloud tops — so
  cloud opacity anywhere on a ground observer's upward ray is generally between them and the beam.
- **Daytime suppression:** reused `sat_flare.comp`'s exact formula (`dayBright = t², t =
  clamp((sunElev+0.05)/0.39,0,1)`) using `pc.sunDirENU.w` (already = sin(solar elevation), no new
  field needed for that half) — only `daySuppression` itself needed adding, one new float in
  `CloudMarchPC` (144→148 bytes), filled from the same `SatelliteSim::daySuppression` member
  `sat_flare.comp` already reads. Applied to `beamRadiance` before the saturating compression.
- **Convergence-point glitches — root cause confirmed by research (not just theorized):** the
  near-parallel closest-point region (`denom→0`) is a fixed ~1° angular window regardless of
  distance — from far away it points toward the satellite (rarely viewed), but from directly under
  a near-vertical beam it sits at the zenith, exactly where the user looks. `tBeamRaw`'s blend was
  already smooth there (follow-up #25); the real remaining instability is `grazingLen`'s steep
  growth (up to `tTop`, tens of km, as `sinTheta→0`) — small viewing-angle changes near the zenith
  then cause large brightness swings. Fixed by fading the boost toward its unboosted baseline
  (`kBeamRayRadiusM`, no amplification) as `targetDist→0` (`grazingFade = smoothstep(0,
  kGrazingFadeRangeM=20000, targetDist)`) rather than touching the underlying math — preserves the
  liked "converges brilliantly from a distance" effect while defusing the close-up gradient.
  **Second, separate contributor:** `kBeamGlowStride=8`'s statistical-averaging assumption needs
  many simultaneous beams to hold; near a single site only a handful are ever relevant, so whether
  a beam survives the coin-flip stride gate (each survivor 8x brighter) caused visible pops as
  satellites cycled through target eligibility. Fixed by exempting nearby beams
  (`targetDist < kBeamStrideBubbleM=30000`) from the stride entirely — evaluated individually, no
  8x compensation — since the existing proximity/range cuts already bound how many such beams can
  exist close to the observer; the stride+compensation scheme now only applies beyond the bubble,
  where the statistical assumption actually holds (many simultaneous distant beams from orbit).
- **Explicitly deferred:** adaptive resolution/supersampling for near-field beam content (the
  half-res dispatch under-resolving a beam that subtends a large screen fraction up close) — a
  plausible secondary contributor per research, not confirmed, much larger change. Revisit if
  glitches persist after the above.

Builds clean (new descriptor bindings + push-constant field + shader logic). **Not yet seen
in-app.**

**Follow-up #29 (2026-07-23) — grazing-angle singularity, altitude cutoff feathering, tunable
extinction, hard-edge softening.**

User confirmed follow-up #28's terrain/cloud occlusion "seem great," and reported four more
issues, all direct refinements of code from this session (no new planning round needed).

- **Low-angle singularity ("overwhelmingly bright beams... before they turn red"), present in
  older versions too:** root cause was follow-up #27's own transmittance-direction fix. `ODR =
  (H_R/mu)*(rhoR - rhoTopR)` needs `(rhoR-rhoTopR)->0` as `mu->0` for the `1/mu` factor to cancel
  into a finite, removable-singularity limit — the ORIGINAL pre-#27 formula achieved this because
  both terms shared the SAME ground reference (`h0`, mu-independent). #27 changed the top reference
  to a FIXED altitude constant (`kBeamGlowMaxAltM`, also mu-independent, but a DIFFERENT constant
  from `rhoR`'s own mu-dependent approach to `rho(h0)`) — breaking the cancellation: as `mu->0`,
  `(rhoR-rhoTopR)` approaches a fixed NONZERO value while dividing by `mu->0` is a true
  (non-removable) singularity. Worse, `mu` can land on EITHER side of zero for a near-horizontal
  target/satellite geometry (pure float noise at that point) — landing slightly negative makes
  `ODR` swing to a huge NEGATIVE number, so `T=exp(-ODR*BETA)` balloons to `>1` (physically
  nonsensical, "overwhelmingly bright") right before the `abs(mu)<=1e-4` fallback branch takes over.
  **Fixed:** evaluate the top reference with the SAME linear `h(t)` model at `t=tTop`
  (`hTop=h0+mu*tTop`) instead of a fixed constant — both `rhoR` and `rhoTopR` now come from the
  same self-consistent model and correctly converge to the SAME value as `mu->0` (a removable
  singularity again, matching the explicit fallback), while still preserving #27's tBeam-to-top
  DIRECTION. **Lesson, extending #26/#27's own:** a real-physics substitution needs to be checked
  for whether it preserves an EXISTING cancellation/limit, not just magnitude and direction.
- **Abrupt altitude cutoff:** the beam segment simply stops existing past `kBeamGlowMaxAltM`
  (25km) with nothing beyond it to fade into — `endFade` only softens by VIEW-RAY overshoot past
  the segment, a different quantity from the evaluated point's own altitude. Added
  `altFade = 1-smoothstep(kBeamGlowMaxAltM-6000, kBeamGlowMaxAltM, h)`, multiplied into the final
  contribution — fades the last 6km of altitude instead of an abrupt stop.
  - **Tunable extinction:** added `beamExtinctionMult` (default 1.0, settings slider "Beam
    extinction," range 0.1-5.0) multiplying the beam's OWN optical-depth exponent
    (`T=exp(-(...)*mult)`) — separate from `BETA_R`/`BETA_M` themselves (stay shared/physical with
    the rest of the atmosphere). `CloudMarchPC` grew 148->152 bytes; `hovCloudMinus/Plus/
    draggingCloud`/`cloudBufs` grew 44->45 for the new slider idx 44, per
    [[feedback_cloud_slider_arrays]].
- **"Hard shell that wraps around the spotlight" near convergence points** — two hard
  discontinuities identified and softened, both literal fixed-radius/boolean boundaries:
  - `perpDist > kBeamRayRadiusM*3.0` was a hard `continue` — a literal tube-shaped wall around
    every beam (proximity is already ~2.6e-18 there, but the BOUNDARY itself was still a true
    discontinuity). Softened into `proximityCutoffFade = 1-smoothstep(3*kBeamRayRadiusM,
    4*kBeamRayRadiusM, perpDist)`, multiplied in; `continue` only past the outer bound.
  - `beamTerrainOccluded()` (follow-up #28) was a hard boolean from a coarse 8-sample test — could
    flip fully-visible/fully-occluded from a tiny viewing-angle change near ground level, its own
    source of sharp edges. Renamed to `beamTerrainVisibility()`, returns a continuous
    `smoothstep(-300, 300, minMargin)` fraction (worst-sample margin, softened over 300m) instead
    of a bool, multiplied into the contribution rather than gating with `continue`.

Builds clean (new push-constant field, member, UI slider, array resizes, shader logic). **Not yet
seen in-app.**

**Follow-up #30 (2026-07-23) — replace the near-field tube with a directional sky-glow bleed.**

User confirmed #29's color/singularity fixes were good, but the near-field experience was still
"weird... sharp edges... beam glow just gets cut in half... a darkening within the middle" and
asked how the tube approach differs architecturally from clouds/aurora's smooth interiors. Answer:
clouds/aurora do real ray marching (many samples integrating actual local density — continuous by
construction); the beam tube reduces each beam to ONE closest-approach point plus closed-form
formulas standing in for a real integral, all built on "camera is far enough away for one point to
represent the whole crossing" — an assumption with no near-field fallback, structurally incapable
of a smooth interior. The user's own reframe: **beams are light, not fog** — a real observer near
one wouldn't see an illuminated cloud, they'd see the sky itself glow brightly with realistic
bleed into the surrounding sky (recalling this project's Light Pollution Dome as the model).

- Researched (2 parallel Explore agents) whether an earlier "directional glow" version of the beam
  effect ever existed (it didn't — every version since the very first has been line/tube geometry;
  the user's memory of "the old sky glow" most likely maps to the ground-hotspot term, which IS
  already a 2D distance-falloff glow, just on the ground plane) and the Light Pollution Dome's
  exact mechanism (`updateLightPollutionDome()`, `SatelliteSim.cpp:4089-4202`; 16-sector array, 2-
  sector interpolation, a FIXED horizon-weighted `elevFalloff` at 4 consumption sites). Concluded
  the dome's specific sector/elevFalloff machinery isn't directly reusable (its elevFalloff assumes
  city lights are always near the horizon; a Reflect-Orbital target can appear at any elevation,
  including zenith, when standing right under one) — but a single `dot(dir, normalize(targetENU))`
  gives a full 2D (azimuth+elevation)-aware falloff directly, simpler than the dome's sector-
  binning-plus-separate-elevation-curve for this specific need. No new SSBO/sector-aggregation
  pass needed — extended the EXISTING per-beam loop instead.
- **Implementation, all in the same beam loop:** one shared `crossfade = smoothstep(0,
  kNearFieldCrossoverM=15000, targetDist)` (0 near, 1 far). The tube's existing shape stack
  (`proximity*grazingLen*endFade*altFade*proximityCutoffFade*terrainVis`) gets an added `*
  crossfade` factor — fades it to zero approaching a beam, which removes the reported artifacts by
  AVOIDANCE (they only occur in the regime now faded out) rather than further patching the tube's
  geometry math. A new bleed term — `bleedFalloff = exp(-(1-cosToTarget)/bleedWidth)` where
  `bleedWidth` grows as the observer nears (mixed by the SAME crossfade, so one parameter drives
  both fades with no gap or double-bright overlap) — reuses the tube's already-computed `T`/
  `hazeColor`/`intensity` (simple, consistent color for a first pass, no separate atmospheric
  model), its own gain (`beamGlowBleedGain`, new tunable, default 0.3, mirrors `beamExtinctionMult`'s
  plumbing exactly — `CloudMarchPC` 152→156 bytes, slider idx 45, arrays 45→46). Deliberately does
  NOT apply `terrainVis` — a directional sky glow isn't blocked by terrain the way a discrete tube
  point is, and its dominant near-field regime makes an intervening mountain implausible anyway,
  matching how the Light Pollution Dome doesn't do per-source terrain occlusion either.

Builds clean (new push-constant field, member, UI slider, array resizes, shader logic). **Not yet
seen in-app.**

**Follow-up #31 (2026-07-23) — observer-relative crossfade fix + beam-driven sky suppression.**

User confirmed #30's crossfade "works well with the sky glow effect from the surface," reported
two more issues, and noted one item of existing behavior (no action needed).

- **Crossfade didn't trigger flying through a beam away from its ground target:** root cause —
  `crossfade` keyed on `targetDist` (distance to the beam's GROUND TARGET only). Flying through
  the middle of a long beam far from where it terminates left `targetDist` large, so the tube's
  near-field artifacts stayed active there. Fixed: `crossfade` now uses a point-to-segment
  distance from `obsPos` to the beam's own clamped 3D line (`obsT = dot(obsPos-targetWorldPos,
  rayDirUp)`, clamped to `[0,tTop]`, then `obsToBeamDist = length(obsPos - closestPoint)`) —
  computed once `targetWorldPos`/`rayDirUp`/`tTop` are known (moved down from where `crossfade`
  used to live, early in the loop). Reduces to ~`targetDist` near the ground end, so this is a
  strict generalization, not a separate measure. `rangeFade` (the separate "in range at all"
  cutoff) correctly still uses plain `targetDist`.
- **Beam sky-glow should suppress satellites/stars/Milky Way like the Light Pollution Dome does:**
  built a SECOND, independent 16-sector dome (`beamGlowDomeBuf`), populated by active beams instead
  of a static night-lights texture, consumed at the same 3 sites the existing dome already is.
  - Written in `sat_orbit.comp`'s existing beam-write block (`targetENU`/`intensity` already on
    hand there): hard-binned by azimuth, `atomicMax(floatBitsToUint(domeVal))` (same idiom as
    `sat_flare.comp`'s flare-sector selection). `intensity` (ground irradiance) does NOT fall off
    with observer distance by physical design — correct for the ground target's own illumination,
    wrong for suppression purposes (a beam on the far side of the planet at the same azimuth as a
    star must not suppress it) — added an explicit `distFade = exp(-targetDist/80000)` plus a
    Reinhard-style compression (`domeRaw/(domeRaw+1.0)`) so the buffer stays bounded to [0,1)
    regardless of how bright a well-aligned mirror gets. New binding 5 on `sat_orbit.comp`.
  - Read (same 16-sector interpolation + `elevFalloff=0.35/(max(z,0)+0.35)` convention as
    `lightDomeBuf`, verbatim) at: `sat_flare.comp` (new binding 4, multiplies `effectFlare`),
    `sat_sky.frag`'s Milky Way section (new binding 19, folded into `visibility`), and
    `updateStars()` (CPU, via a one-frame-stale readback into `beamGlowDomeAz[16]` — same
    `HOST_VISIBLE|HOST_COHERENT` + mapped-pointer-read-before-this-frame's-fill idiom as
    `reflectBeamsBuf`/`glowBuf`). Each site gets its own `kMaxDim` cap, same pattern as the
    existing dome's per-consumer caps.
  - Deliberately a SEPARATE buffer from `lightDomeBuf`, not merged — two independent phenomena
    that happen to share a consumption shape.
- **Item 3 (mirrorFrac alignment triggering `sat_flare.comp`'s own lens flare/sky illumination)**
  was a user note about existing, correct behavior — no action taken.

Builds clean (3 shaders + C++ across 2 descriptor sets, 1 new buffer, 1 new CPU readback). **Not
yet seen in-app.**

**Follow-up #32 (2026-07-23) — beam-dome scale fix + low-elevation "white ray" fix.**

User reported the sky glow itself is visible, but the Milky Way isn't visibly dimming near it, and
a separate artifact: exiting/retargeting satellites near the horizon suddenly cast "a huge,
uniformly white ray" toward their target — asked for all beam elements to fade out above a small
elevation margin (~5°).

- **Beam-dome suppression too weak (likely everywhere, not just the Milky Way — just least
  noticeable against a broad diffuse band vs. a point source):** worked the actual numbers —
  `kBeamDomeScale = 3e-8` (follow-up #31's first-pass guess) combined with a well-aligned Reflect
  Mirror's peak `intensity` (~3.1e6, from `crossSectionM2=2376`, `mirrorFrac=0.97`) gives
  `domeRaw ≈ 0.09` even at point-blank range — nowhere near enough to compete against
  `kBeamDomeCompressK=1.0`'s denominator (`domeVal ≈ 0.086`, an at-most ~8% dim with
  `kMaxDim=0.99`). Retuned `kBeamDomeScale` to `2e-6` (~67x) so a well-aligned close mirror reaches
  `domeRaw ≈ 6.3` (`domeVal ≈ 0.86`) — a real, visible suppression, matching the dynamic range the
  actual Light Pollution Dome achieves for a bright city.
- **Low-elevation "white ray" — root-caused, not just patched:** at very low satellite elevation
  the geometry goes doubly degenerate — the view ray can graze nearly the ENTIRE beam length at
  once (an extreme oblique/near-parallel viewing angle, which `grazingLen`'s boost amplifies
  heavily), while the visible portion of the segment sits mostly near the TOP of the segment (near
  the satellite, near space, negligible atmosphere) where `T` is still ~(1,1,1) — barely reddened.
  Boosted brightness + no color = an oversized, colorless flash, worst right as a satellite is
  rising/setting or its mirror is mid-retarget. Rather than chase every such edge case in the
  tube's closed-form math (the same lesson as #25-29's tube saga), added a blunt, robust,
  physically-reasonable gate instead: `satElevSin = normalize(beams[bi].satENU).z`,
  `elevFade = smoothstep(0, sin(5°), satElevSin)`, multiplied into BOTH the tube and bleed
  contributions (and an early `continue` when `elevFade<=0`, i.e. at/below the horizon) — a real
  low-elevation satellite would be realistically near-fully-extinguished anyway.
- **Build-system gotcha hit this session, worth remembering:** `sat_orbit.comp`'s `.spv` was
  timestamped NEWER than its just-edited source (`ls -la` showed the source at an OLDER mtime than
  its own compiled output — a Windows/tool-chain clock quirk, not a real content issue), so the
  glob-based shader-compile step silently skipped recompiling it despite the real content change.
  Caught by comparing `ls -la --time-style=full-iso` on both files after a build that suspiciously
  only compiled one of two edited shaders; fixed with `touch` on the source before rebuilding.
  **If a future build only recompiles SOME of the shaders you just edited, check timestamps before
  assuming the others had no changes.**

Builds clean (verified with a follow-up clean build showing zero pending shader recompiles).
**Not yet seen in-app.**

**Follow-up #33 (2026-07-26) — real beam↔cloud occlusion: audit found three separate gaps,
fixed with a new per-target GPU pass instead of touching the tube's own math.**

User audit request ("beams should be cut off by clouds, not hit the ground") led to auditing all
three places a beam could interact with cloud, finding each was in a different state:

1. **Ground-spot term (`sat_sky.frag`) — real, previously-undiagnosed bug.** Its `shadowAtten`
   sampled `cloudShadowTex`, the general-purpose cloud-shadow grid (`cloud_shadow.comp`) — 80km
   half-extent (`cloudShadowRangeM`), centered on the OBSERVER. Beams render out to `beamMaxRangeM`
   (500km). Any target beyond 80km — the common case, since the 201 reflector targets are
   scattered globally at ~1500km+ typical spacing — always fell outside the grid
   (`tShadowEdge > 1.0` collapses `tShadowFade` to 0) and got `shadowAtten = 1.0`: **no shadow,
   silently, for most of the beam's actual visible range.**
2. **Sky-glow tube/bleed (`cloud_march.comp`) — not literally inert, but the wrong model.** Already
   multiplied by `A_total` (this pixel's own view-ray cloud transmittance, follow-up #28) — some
   real dimming happens — but it's a "uniform dim by whatever cloud sits anywhere along this ray"
   approximation with **no depth-awareness**, explicitly documented as accepted. It was never built
   to make a beam visually terminate at a cloud, which is what "cut off, not hitting the ground"
   actually asks for.
3. **Satellite/mirror lens flares (`sat_flare.comp`, `sat_point.vert/frag`) — zero cloud awareness
   anywhere,** confirmed by direct grep (no "cloud" string in any of the three files). Never built,
   for any satellite, not just mirrors.

User chose to fix #2 (a real redesign) and #3 (new capability) — #1 got fixed anyway, as a required
side effect of #2's design (see below), even though it wasn't separately selected.

**Landmine avoided:** follow-ups #14-#16 (see that section, three entries below) tried exactly this
kind of real cloud-aware beam lighting and were fully reverted — root causes were (a) marching real
`cloudDensity()` **once per satellite** (up to 2048, when many satellites can share one busy
target), scaling both cost and brightness with satellite count instead of target count; (b) the
dedup fix anchored *geometry* to whichever satellite won a per-frame brightness arbitration, and
the winner flipping frame-to-frame among near-equal satellites caused visible flicker. This
round's design avoids both structurally: the new lookup below is a pure function of **ground
target position + current cloud field only** — no satellite identity or arbitration anywhere in
it — computed exactly once per unique target (≤201) regardless of how many satellites use it, so
there's nothing to flicker between.

**Design: a new tiny compute pass, `shaders/beam_cloud_block.comp`, evaluated per-target, not
per-satellite or per-screen-pixel.** For each of the 201 fixed reflector targets: a 12-step
vertical (straight-up, sun-independent) march through the main cloud shell (`layers[0]`/`[1]`,
same bounds `cloudMarchCS` uses), using the exact hand-duplicated `cloudDensity()`/`raySphere()`/
noise-sampling helpers `cloud_shadow.comp` already uses (same project convention as
`[[feedback_shared_ubo_duplication]]` — no GLSL include mechanism exists to share this directly).
Outputs two floats per target into a new `beamCloudBlockBuf` (device-local, 201×vec2, no atomics —
each of the 201 threads owns exactly one index): `blockAltM` (altitude at which vertical
transmittance first drops below 50%) and `blockOpacity` (0=clear column, 1=fully opaque).

**Deliberately NOT built on `cloud_shadow.comp`'s existing grid** — mid-session the user flagged
that grid as "in a bad spot... a singular square pixelated texture that follows the observer and
does not track well for high vantage points," and asked whether this plan depended on it. It
doesn't: the new buffer evaluates each target's exact ECEF position directly, no spatial grid, no
interpolation, no observer-centered pixelation to inherit — only the low-level march *technique* is
borrowed, not the grid architecture. If the general shadow grid is redesigned later, this pass is
fully decoupled and needs no changes.

**A new static buffer, `reflectorTargetsECEFBuf`** (201×vec4, xyz=unit ECEF dir, w=ground radius),
uploaded ONCE (right after `initConstellation()` in `init()`, since `reflectorTargetsECEF[]`/
`reflectorTargetsRadiusM[]` never change afterward) — needed because cloud lon/lat sampling wants
true ECEF, and the existing `reflectorTargetsBuf` is ECI (rotates with GMST every frame); avoids
adding GMST-rotation math to the new shader.

**Consumption: baked directly into `ReflectBeam` (grown 48→64 bytes)** rather than adding a new
binding to `cloud_march.comp`/`sat_sky.frag` — both already read `beams[bi]` per-beam, so
`sat_orbit.comp` (new binding 6, reads `beamCloudBlockBuf[bestIdx]` at its existing beam-write site)
just copies `blockAltM`/`blockOpacity` straight onto the entry being written; no `bestIdx`
indirection needed downstream. All three `ReflectBeam` copies (`sat_orbit.comp`, `cloud_march.comp`,
`sat_sky.frag`) and the C++ `GpuReflectBeam`/`static_assert` updated together.

- **`cloud_march.comp`: a new `cloudFade` term next to the existing `altFade`, NOT a change to
  `tTop`/the closest-approach solve/extinction math.** That region has this file's worst bug
  history (follow-ups #24-#29 all landed real bugs there) — a pure brightness fade reusing the
  ALREADY-per-pixel `h` (the closest-approach point's own altitude) is lower-risk and sufficient:
  `cloudFadeHard = smoothstep(blockAltM-500, blockAltM, h)`, blended with `blockOpacity` via `mix`
  so `blockOpacity==0` (no cloud) is a true no-op regardless of `blockAltM`'s value. Multiplied into
  both the tube's and the bleed's `beamRadiance +=` lines. Since a beam drawn as a line in the sky
  has different `h` at different screen pixels, this reads as the beam visibly terminating right
  where it meets the cloud (brightest at the cutoff, since `proximity` is centered there) while the
  portion toward the satellite stays fully lit — and, as a side effect, partially covers the
  original "illuminate the cloud" ask for free (no separate lit-patch-on-the-cloud-surface work
  attempted this round).
- **`sat_sky.frag`: ground-spot's `shadowAtten` replaced outright** with `1.0 - beams[bi].blockOpacity`
  — deletes the old 80km-limited `cloudShadowTex` lookup entirely for this term (fixing bug #1
  above as a required side effect: the sky cutoff and the ground spot must agree, or a beam could
  visibly terminate overhead while its landing point stayed lit).
- **Satellite/flare cloud occlusion: `sat_point.frag` reads cloud data for the first time.** New
  bindings 5/6 (`cloudTargetA`/`cloudTargetB`, same underlying views `skyDescSet` already binds at
  10/11) on the satellite draw's own 5→7-binding `descLayout` (a separate, smaller set from
  `sat_sky.frag`'s 20-binding `skyDescLayout` — confirmed via research, not assumed). Push constant
  range grew `VERTEX`→`VERTEX|FRAGMENT` (`drawPipeLayout`, plus the matching
  `vkCmdPushConstants` stage-flags at the actual draw call — Vulkan requires these to match the
  layout's registered range exactly), with `sat_point.frag` declaring just the fields it needs
  (`screenSizePx` at its real offset 136) as a longer prefix of the shared `SatDrawPC` block,
  the same "declare only what you use, real byte offsets" trick `sat_point.vert` already uses for
  its own (shorter, stale-comment) prefix. `cloudUV = gl_FragCoord.xy / pc.screenSizePx` — replicated
  exactly from `sat_sky.frag`, not an assumed constant (the documented render-scale gotcha).
  Two-tier response mirroring how `sat_sky.frag` already treats cloud opacity elsewhere: a hard
  zero for genuinely opaque cloud (`tCloudOcclude`, same convention that already hides the moon
  disc) times a smooth power-curve dim otherwise (same shape the Milky Way/sun disc already use) —
  so satellites join the existing visual language rather than a new one.
- **Real bug caught before it shipped: `onResize()` didn't refresh `descSet`'s new bindings 5/6.**
  The half-res cloud targets get destroyed/recreated on every resize, and `onResize()` already
  patches `skyDescSet`'s (10/11) and `cloudMarchDescSet`'s (5/6) image-view writes to match — but
  had no equivalent for the satellite draw's own `descSet`, which would have kept pointing at freed
  image views after the first resize. Added the same refresh pattern for `descSet` bindings 5/6.

**Files touched:** `shaders/beam_cloud_block.comp` (new), `shaders/sat_orbit.comp` (binding 6,
`ReflectBeam` growth, write site), `shaders/cloud_march.comp` (`ReflectBeam` growth, `cloudFade`),
`shaders/sat_sky.frag` (`ReflectBeam` growth, ground-spot rewrite), `shaders/sat_point.frag` (new
bindings/push-constant/cloud response), `src/simulations/SatelliteSim.h` (`GpuReflectBeam`→64
bytes, `BeamCloudBlockPC`, new buffer/pipeline/descriptor members), `src/simulations/SatelliteSim.cpp`
(buffer creation, `createBeamCloudBlockDescriptors`/`Pipeline`, `recordCompute()` dispatch+barrier
before `sat_orbit.comp`, `createOrbitDescriptors()`→7 bindings, `createDescriptors()`→7 bindings,
`createDrawPipeline()`/push-constant stage flags, `onResize()` fix, one-time ECEF upload after
`initConstellation()`).

Builds clean (all five touched shaders recompiled and linked with no errors; verified `.spv`
timestamps are newer than sources, learning the lesson from follow-up #32's build-timestamp
gotcha). **Not yet seen in-app** — this round's tuning constants (`kCloudFeatherM=500`,
`kSatCloudSuppressPower=2.0`, the 50%-transmittance threshold for `blockAltM`) are all first-pass
guesses, same as every other C12 constant on first landing.

**Follow-up #34 (2026-07-26) — physically-derived beam radius, replacing two flat constants with
one shared physics formula.**

User asked how large beams currently are and how to make their size derive from the satellite's
own area, emphasizing accuracy. Both existing size quantities were completely disconnected from
physics: the ground footprint (`beamFootprintRadM`, a settings slider, default 50,000 m radius,
range 500m–200km) was a single flat value identical for every satellite regardless of mirror size
or altitude, and the sky tube's visual thickness (`kBeamRayRadiusM = 3000.0` in `cloud_march.comp`)
was a separate hardcoded constant, also identical for every beam.

**Root physics, worked out from scratch:** for a large flat mirror reflecting sunlight, the
dominant spreading mechanism is **not diffraction** off the mirror's own aperture (~1e-8 rad for a
~48m mirror — utterly negligible) — it's that **the sun itself isn't a point source**. Its mean
angular radius (~0.2665°, 0.0046505 rad) means even a perfectly flat mirror reflects a cone of that
half-angle, so the beam widens as it travels regardless of how collimated the mirror itself is.
Using the sun's fixed mean angular radius (not a date-accurate one factoring in Earth's orbital
eccentricity) matches the sim's existing level of fidelity — it already treats `SOLAR_CONSTANT` as
a fixed 1361 W/m² regardless of simulated date, so this isn't a new simplification being
introduced. One formula, used at both ends with a different propagation distance:
```
radius = mirrorRadiusM + distanceFromMirrorM * tan(kSunAngularRadiusRad)
mirrorRadiusM = sqrt(mirrorAreaM2 / PI)   // equivalent-circle radius of the physical mirror
```
For the largest mirror type (2376 m², ~500km typical range) this gives ~2-3km radius — the mirror's
own size barely matters (~27m), the range term dominates completely. This is ~20x smaller than the
old flat 50km default, which is why "how do we scale them down" and "make it physically derived"
turned out to be the same fix.

- **Ground footprint (`sat_orbit.comp`, at the existing beam-write site):** `mirrorAreaM2` was
  already computed there for `groundIrradiance`, and `satENU`/`targetENU` were already both
  resolved — no new inputs needed. `mirrorRadiusM = sqrt(mirrorAreaM2/PI)`, `slantRangeM =
  length(satENU-targetENU)` (true 3D mirror→target distance, not just altitude),
  `footprintRadM = mirrorRadiusM + slantRangeM*tan(kSunAngularRadiusRad)`. Also fixed a stale
  comment nearby that had (incorrectly, and not actually implemented that way) asserted "the
  reflected beam's footprint tracks the mirror's own size, not an expanding point-source cone" —
  conflating two separate physical facts: ground IRRADIANCE genuinely doesn't fall with range for
  an idealized collimated reflector, but the footprint's SIZE does grow with range regardless.
- **Sky tube thickness (`cloud_march.comp`) — now per-pixel, not a flat constant.** The tube math
  already resolves `tBeam`/`p` (the per-pixel closest-approach point along the beam's segment)
  before any use of the old `kBeamRayRadiusM`. Computed right after that point: `tSatParam =
  length(beams[bi].satENU-beams[bi].targetENU)` (same line `rayDirUp` already follows),
  `distFromSatM = max(tSatParam-tBeam, 0)` (distance already traveled from the SATELLITE, not the
  ground — divergence accumulates from the source outward, narrowest near the mirror, widest near
  the ground), `tubeRadiusM = max(beams[bi].mirrorRadiusM,1) + distFromSatM*tan(kSunAngularRadiusRad)`.
  At `tBeam=0` this is exactly the ground footprint's own formula — the tube's width at the ground
  now matches the ground-spot's footprint exactly, instead of an unrelated flat constant. Replaced
  all six existing uses of `kBeamRayRadiusM` (`proximityCutoffFade`, `proximity`,
  `sinThetaClamped`, `grazingLenBoost`, `grazingLen`, `endFade`) with `tubeRadiusM` — a mechanical
  substitution only; deliberately did NOT touch the segment/closest-approach geometry itself (that
  region has this file's worst bug history, follow-ups #24-29).
- **Ground-spot core (`sat_sky.frag`):** `coreR = footprintR*0.15` (an arbitrary ratio) became
  `coreR = max(beams[bi].mirrorRadiusM, 1.0)` — the tight bright "hotspot" is now the mirror's own
  true physical size, with the existing soft halo (`footprintR`, unchanged) representing the full
  sun-disk-broadened extent around it.
- **`ReflectBeam` grows by repurposing padding, not by growing the struct.** `pad0` (present in all
  three GLSL copies + the C++ struct since follow-up #33's growth to 64 bytes) is now
  `mirrorRadiusM` — zero size change, same trick already used for `debugPad`.
- **Manual override removed entirely, per explicit user decision** (asked via a scope question: keep
  a scale-multiplier slider on top of the physics, vs. remove the free parameter outright — user
  chose removal as the simplest, most direct match for "accuracy is important"). Deleted:
  `SatOrbitPC::beamFootprintRadM` (struct shrinks 108→104 bytes, matching GLSL's own implicit
  sequential push-constant offsets on the other side), `SatelliteSim::beamFootprintRadM` CPU member
  and its `orbitPc.beamFootprintRadM = ...` assignment, the `"beam_footprint_rad_m"` settings.json
  load/save keys, and the "Beam footprint (m)" slider row. **Per
  `[[feedback_cloud_slider_arrays]]`: did NOT resize `hovCloudMinus`/`hovCloudPlus`/`draggingCloud`
  or renumber any other slider's index** — each row already carries its own explicit index number
  (not positional), so deleting this one row simply leaves index 40 permanently unused, same
  accepted "dead capacity" pattern already documented for `reflectorTargetsECEF`'s indices 0/200.

**Files touched:** `shaders/sat_orbit.comp` (footprint formula, `mirrorRadiusM` write, removed
push-constant field, stale-comment fix), `shaders/cloud_march.comp` (`tubeRadiusM` replacing
`kBeamRayRadiusM` at 6 call sites), `shaders/sat_sky.frag` (`coreR` from `mirrorRadiusM`),
`src/simulations/SatelliteSim.h` (`SatOrbitPC` field removal, `GpuReflectBeam` comment update,
`beamFootprintRadM` CPU member removal), `src/simulations/SatelliteSim.cpp` (push-constant
assignment removal), `src/simulations/SatelliteSimUI.cpp` (slider row + settings.json key removal).

Builds clean (all three touched shaders recompiled, C++ compiled/linked with no errors; `.spv`
timestamps verified newer than sources). **Not yet seen in-app** — expect beams to look
noticeably narrower/tighter than before, especially far from their target, and the ground-spot's
bright core to read as a small sharp point rather than a large diffuse blob.

**Follow-up #34 fix (2026-07-26, same day) — user report after seeing #34 in-app: a hard edge
tracking the satellite's position, plus low-angle beams cutting off wrong.**

User reported three things at once: (1) a hard edge within the beam that visibly tracks around the
satellite's position, and asked directly whether the crossfade/near-field-bleed handling from
follow-up #30/#31 had regressed; (2) low-elevation beams not casting properly — the beam's edge
appeared to get cut off at the same ground distance as the center, when it should paint more of an
oval and the higher-altitude part of the beam should continue past the target rather than
terminating early; (3) the ground spot no longer visible at all.

**#1 root-caused as a genuine regression from #34, not the crossfade feature going away.**
`tTop` (the tube segment's far end) was computed purely from `raySphere` against the FIXED 25km-
altitude sphere (`kBeamGlowMaxAltM`), with no relationship to the satellite's own TRUE distance
(`tSatParam = length(satENU-targetENU)`). At low elevation this genuinely diverges: `kBeamGlowMaxAltM`
is an ALTITUDE, but at shallow `mu` (near-horizontal beam) the PATH LENGTH needed to gain that same
25km of altitude balloons — e.g. `mu=0.01` needs 2500km of path length for 25km of altitude gain,
easily more than a typical several-hundred-km LEO slant range. Unclamped, the tube rendered a
fictitious continuation PAST the real satellite position. Follow-up #34's own `distFromSatM =
max(tSatParam-tBeam, 0)` then clamped to exactly 0 across that fictitious continuation, collapsing
`tubeRadiusM` down to just `mirrorRadiusM` (~27.5m) there — an abrupt, near-zero-width pinch whose
location depends on the satellite's current distance, reading exactly as "a hard edge that tracks
around the satellite point" as the satellite moved frame to frame. This is a real bug this session
introduced (the crossfade/bleed feature itself is untouched and still there), not a lost feature.
**Fix:** `tTop = min(topHit.y, tSatParam)` — computed once, right after `rayDirUp`, before `tTop`'s
first use — the tube segment can never extend past the satellite's actual position, which also
means `distFromSatM` is now provably `>= 0` always (the `max(...,0)` clamp on it is now dead code
in practice, left in place as a defensive no-op). This is also a genuine pre-existing correctness
fix independent of the visual symptom: the beam should never have rendered past the real satellite.

**#3 (ground spot invisible) investigated and found NOT to be a bug** — it's the direct, expected
consequence of #34's own accuracy fix: the halo shrank ~20x (50km→2-3km) and the core ~270x
(7.5km→27.5m, the mirror's true physical size). At typical viewing distances the landing point is
now genuinely easy to miss unless positioned almost exactly over the target. Asked the user
directly (scope question: leave it fully physical, vs. add an artificial minimum-visibility floor
on top) — **user chose to leave it fully physical**, no change made.

**#2 (low-angle footprint shape) is real missing physics, not a bug** — spun into its own follow-up
below (elliptical ground footprint) since it required actual geometric derivation, not a one-line
fix.

Builds clean (`cloud_march.comp` only, single-file change). **Not yet seen in-app.**

**Follow-up #35 (2026-07-26, same day) — elliptical ground footprint for grazing beams.**

Addresses #34-fix's item #2 above. Worked the paraxial cone-plane intersection geometry from
scratch (justified since the divergence half-angle — the sun's angular radius, 0.0046505 rad — is
tiny, so first-order/small-angle terms dominate completely): a circular cone of half-angle α with
apex at the satellite, axis hitting the ground at elevation angle θ, produces an ellipse that is,
to leading order in α, **unchanged in the across-track direction** (perpendicular to the beam's
ground-projected azimuth) and **stretched by exactly `1/sinθ` along the track** (toward/away from
the point on the ground directly below the satellite) — the same "flashlight on a tilted wall"
intuition. Crucially, **worked out that it's symmetric, not asymmetric, to this order** — the
initial hand-wavy guess that the far edge should stretch more than the near edge only shows up at
SECOND order in α (~0.5% relative correction here — imperceptible for a sun-angular-radius-driven
beam), so a plain centered ellipse is the physically correct model, not an offset one. This also
means `footprintRadM` itself (computed in `sat_orbit.comp`, follow-up #34) needed NO new formula —
it already IS the correct minor-axis (unstretched) radius exactly as derived; only the ground-spot
term's *consumption* of it in `sat_sky.frag` needed to become anisotropic.

Implementation, entirely inside `sat_sky.frag`'s ground-spot loop (no buffer/struct changes — every
needed input, `satENU`/`targetENU`/`footprintRadM`/`mirrorRadiusM`, was already on `beams[bi]`):
`sinElev = normalize(beams[bi].satENU-beams[bi].targetENU).z` (the same `mu` quantity
`cloud_march.comp` already computes for its own altitude formula, just never computed here before);
decompose `hitPt.xy - targetENU.xy` into along-track/across-track components via the ground-
projected azimuth direction (`beamDirUp.xy`, normalized, with a guard for near-vertical beams where
that projection is ~zero-length); `majorR = footprintR/sinElev`, `coreMajorR = coreR/sinElev`
(minor axes unchanged); swap the single-sigma Gaussians for the two-sigma anisotropic equivalent.
Also bundled in an `elevFade` (same `smoothstep(0, sin(5°), sinElev)` shape `cloud_march.comp`
already applies to the sky tube/bleed, follow-up #32) — needed because without it, an extremely
grazing beam's ellipse stretches toward infinity as `sinElev->0` with nothing to cut it off,
inconsistent with the sky portion already vanishing at that same elevation, and would have been a
second independent source of "beams don't cast properly at low angles" left unaddressed.

Builds clean (`sat_sky.frag` only, single-file change, no C++/struct side effects). **Not yet seen
in-app** — expect a near-circular spot for high-elevation beams (unchanged) and a visibly elongated
oval, stretched toward/away from the satellite's ground-projected direction, for low-elevation
ones, fading out entirely below ~5° satellite elevation.

**Follow-up #36 (2026-07-26, same day) — fixed the bleed term's structural gating bug ("hollow
tunnel"), and extended the beam segment past the target for grazing hits.**

User tested #35 and reported two things: (1) looking into a beam shows a dark, hollow-cylinder
interior with harsh edges, worst near the ground where it meets terrain/ocean — asked directly
whether the crossfade/near-field-bleed handling (follow-up #30/#31) had been lost; (2) with the
ground now an elongated ellipse, the sky beam visibly stops dead at the target point instead of
continuing to meet the ellipse's far edge — proposed extending the beam based on its width and
grazing angle so the beam's own far edge is what clips the ground, not the infinitesimal axis.

**#1 root-caused by re-reading the live loop end-to-end (not relying on memory) — a real structural
bug from this session's own restructuring, not the bleed feature regressing.** The near-field bleed
(follow-up #30) sits AFTER three tube-specific early `continue`s: `sView<0` (closest-approach point
on the infinite line falls behind the camera), `proximityCutoffFade<=0` (beyond ~4σ of the tube's
radius), `terrainVis<=0` (that specific 3D point locally terrain-occluded). All three abandon the
**entire loop iteration** — so whenever any trips, bleed never runs either, even though bleed is a
pure angular falloff that needs none of those quantities. Worse, bleed **reuses** `T`/`hazeColor`,
which were computed even later, inside the same gated region — so the fix couldn't be "just delete
the `continue`s," `T`/`hazeColor` needed to stop depending on that region too. Standing near/inside
a beam and looking sideways or downward routinely produces exactly `sView<0` or `terrainVis<=0` —
not edge cases — which is exactly the "hollow tunnel"/harsh-edges/harsh-terrain-intersection
symptom: precisely the directions where bleed should smoothly fill in were silently rendering
nothing.

**Fix: reorder, don't reformulate.** `h0`/`mu`/`h`/`altFade`/`cloudFade`/`rhoR`/`rhoM`/`hTop`/
`ODR`/`ODM`/`T`/`hazeMagnitude`/`hazeColor` only ever depended on `tBeam` and `beams[bi]` fields —
never on `sView`/`perpDist`/`proximityCutoffFade`/`terrainVis` — they were just declared later than
necessary. Moved that whole block to right after `tBeam`/`p`, before the `sView` check — zero
formula changes, just relocation — making `T`/`hazeColor` available to bleed unconditionally. The
three `continue`s became a single multiplicative gate on the TUBE's own contribution only
(`tubeVisible = (sView>=0)?1:0`, combined with `proximityCutoffFade`/`terrainVis` which were
*already* continuous multipliers in the tube's final formula — only their early-exit shortcut was
removed, not their own math). Bleed, computed unconditionally afterward, is now genuinely
independent, as its own (previously aspirational) comments already claimed.

**#2: extended the segment's lower bound using the same paraxial geometry as #35's ellipse, solved
for path length instead of ground displacement.** A finite-width beam's far edge must travel
farther than the central axis before reaching ground level at a shallow angle:
`extraLengthM = footprintRadM * cosElev / sinElev` (sinElev=mu, cosElev=length(rayDirUp.xy)) — 0 at
θ=90° (straight down), growing large at grazing incidence, matching "the beam should continue past
the target." `tBeam = clamp(tBeamRaw, -extraLengthM, tTop)` (was `clamp(..., 0.0, tTop)`), and
`endFade`'s overshoot formula updated to reference the new lower bound instead of a hardcoded 0.
The altitude formula's existing `max(0.0, ...)` floor means the new negative-`tBeam` stretch simply
evaluates at ground-level atmospheric density/color — a deliberately simple approximation (not a
rigorous independent re-trace of the true 3D edge ray), reasonable for what's fundamentally a
visual patch connecting the tube to the already-approximate ellipse.

Both fixes touch the same loop and interact (fix #1's reordering is what makes `mu` available early
enough for fix #2's `extraLengthM`), so landed together in one pass. No struct/buffer/push-constant
changes — everything needed (`footprintRadM`, `mirrorRadiusM`, `satENU`, `targetENU`) was already on
`beams[bi]`.

Builds clean (`cloud_march.comp` only, single-file change; `.spv` confirmed newer than source).
**Not yet seen in-app** — expect smooth, continuous glow when standing near/inside a beam (no dark
hollow interior, no hard edges at terrain/ocean), and a low-elevation beam's sky portion to visibly
extend past the target to meet its elongated ground ellipse instead of stopping short.

**Follow-up #37 (2026-07-26, same day) — extension-underground bug fix, and the tube-approximation
question raised directly.**

User reported the tube's hollow/cylinder nature was "even more apparent now," asked directly why
beams are rendered as tubes at all rather than continuous beams, and reported #36's ground extension
was only visible from above — moving to sea level made it "shrink inward."

**Root-caused the sea-level bug: a real geometric error in #36's own extension.** `p = targetWorldPos
+ rayDirUp * tBeam` continues along the beam's full 3D axis for negative `tBeam` — but `rayDirUp` has
an UPWARD component (`mu`), so extending "backward" along that same tilted line doesn't stay on the
ground, it dives underground by `mu*|tBeam|`. The altitude formula (`h`) already floored at 0 so
brightness/color looked fine, but `p`'s actual 3D POSITION was never corrected, and that position
feeds `beamTerrainVisibility(obsPos, p, ...)` — from a low, sea-level vantage, the sightline to a
now-underground point gets correctly blocked by terrain occlusion; from high above, the discrepancy
is small enough relative to viewing distance to not be obvious. Fixed by splitting the position
formula at `tBeam=0`: the normal segment (`tBeam>=0`) is unchanged; the extension (`tBeam<0`) moves
along a purely HORIZONTAL direction (`horizontalDirUp`, the ground-projected component of
`rayDirUp`) instead of continuing along the tilted axis, staying on the ground instead of diving
under it. `h`'s formula updated the same way (`mu * max(tBeam,0.0)` instead of `mu*tBeam`) so the
extension's altitude stays flat at the target's own `h0` instead of decaying toward sea level. Both
halves agree exactly at `tBeam=0` (zero displacement either way), so no new seam introduced.

**The architecture question was answered directly, not deflected.** Explained why tubes exist at
all: a real volumetric march for beams was tried FIRST (follow-up #14) and reverted — performance
collapsed near busy sites because Reflect-Orbital's "Disk" deployment can have dozens of satellites
converging on one target, and marching each independently multiplied cost by satellite count, not
target count (see that follow-up's own section, and lesson 5 in the memory file, for the full
history). The analytic "tube" replaced it: an O(1)-per-beam single-closest-point evaluation instead
of marching, cheap enough for 2000+ simultaneous beams, at the cost of having no real volumetric
interior — which is the root cause behind essentially every tube-specific artifact fixed this
session (hollow tunnel, hard edges, the extension bug above). Presented three options (keep
patching the analytic model / a real but scoped volumetric march for near-field only / simplify the
visual model to something structurally immune to these artifacts) — **user chose the scoped
near-field volumetric march.** Full design for that is its own follow-up (see below); this entry is
just the extension bug fix + the decision record.

Builds clean (`cloud_march.comp` only). **Not yet seen in-app.**

**Follow-up #38 (2026-07-26, same day) — the scoped near-field volumetric march, designed and
shipped.**

Full design per the decision recorded in follow-up #37. Replaces the near-field "bleed" term's
FORMULA only — same trigger (`crossfade<1`), same gain slider (`beamGlowBleedGain`), same slot in
the loop — swapping a pure angular falloff (no depth, uniform brightness regardless of how much of
the beam's volume a given view ray actually passes through) for a short, genuinely depth-aware
march through the tube's own Gaussian cross-section.

**Avoids both of follow-up #14's specific failure modes, explicitly:** (1) only activates in the
near field (within `kNearFieldCrossoverM=15000m` of a beam's own line) — the far-field tube, the
common case, is untouched; (2) pure ALU, zero texture samples (a closed-form Gaussian
distance-to-line evaluation per step, not a `cloudDensity()` lookup) — the same
cheap/expensive distinction follow-up #17 already drew in this file between the debug pointing rays
(ALU-only, cheap at any sample count) and #14's reverted texture-heavy per-satellite march.

**March range reuses `sinThetaClamped`** (already computed by the tube's own `grazingLenBoost`)
instead of deriving a new closed-form ray-vs-cylinder intersection — `tubeRadiusM/sinThetaClamped`
was already "how far along the view ray you stay within one sigma of the tube," so
`marchHalfRangeM = tubeRadiusM * 3.0 / sinThetaClamped` centered on `sView` (the tube's own
closest-approach point — correct by construction) is a natural, low-risk reuse rather than new
math in the file's most bug-prone region.

**The march loop** (10 fixed steps, `kBeamMarchDensityScale=1.5e-4` first-pass constant): for each
step, project the sample point onto the beam's own axis (`tSample`), skip if outside the segment's
valid range (`[-extraLengthM, tTop]`, follow-up #36/#37's own extended bounds), evaluate the SAME
Gaussian density formula `proximity` already uses (held at the closest-approach point's own
`tubeRadiusM` for the whole march — taper across a near-field-scale span is minor, dominated by
distance-from-satellite which barely changes there), accumulate transmittance/scatter in the same
shape `cloudMarchCS` already uses elsewhere in this file (`marchT *= stepT; marchAccum += marchT*(1-stepT)`).
Reuses `T`/`hazeColor` already computed once at the closest-approach point for the tube term, rather
than re-deriving per march step (same "near-field span is short enough that atmospheric properties
barely change" reasoning).

**Cost risk at a busy convergence site (many beams simultaneously near-field) is accepted, not
preemptively mitigated** — flagged explicitly in the plan rather than guessed at: unlike #14 this is
pure ALU so per-step cost is far cheaper, but a busy site with dozens of near-field beams would
still run this march that many times. If real profiling shows this is a problem, `kBeamMarchSteps`
and the already-established `kBeamStrideBubbleM`/stride pattern (used elsewhere in this same loop)
are the two knobs to reach for — not addressed speculatively this round.

No struct/buffer/push-constant changes — every input (`tubeRadiusM`, `sinThetaClamped`, `sView`,
`targetWorldPos`, `rayDirUp`, `tTop`, `extraLengthM`, `T`, `hazeColor`) was already computed
unconditionally earlier in the same loop iteration (follow-up #36's own reordering is what made
this possible without any further restructuring).

Builds clean (`cloud_march.comp` only; `.spv` confirmed newer than source). **Not yet seen
in-app** — expect standing near/inside a beam to now show genuine depth (brighter through the
tube's center, dimmer at its grazing edge) instead of a flat angular glow; far-field beams should
be completely unaffected; cost at a busy multi-satellite site is the one open question that needs
real in-app observation, not just code review.

**Follow-up #39 (2026-07-26, same day) — the near-field march didn't pan out; removed entirely,
replaced with a sky-illumination wash instead of any per-pixel volumetric geometry.**

User tested #38 and reported: no visible march despite real performance cost, even after tuning
sigma range (3.0→7.0), density scale (1.5e-4→1.5e-1), and step count (10→20) — none of it made the
march show up. Combined with #37's extension also not clearly working, the user's own conclusion:
trying to render something that looks like a 3D tube's interior is probably the wrong path,
performance-wise and results-wise, on top of already being #23-38's long chain of patches to the
same single-point/point-sampled model. **Proposed replacement, explicit:** if the observer is
within a beam, don't render the beam's own shape there at all — let it fade to nothing (which
`crossfade` already does to the tube) — and sell "standing in an intensely lit spot" via (1) the
satellite's own point-sprite flare, already independently rendered and already effective when
aligned, and (2) a general, non-directional "tremendous sky illumination," explicitly NOT shaped
like the beam's geometry.

**Found an exact, already-proven precedent for (2) already in `sat_sky.frag`: city light-pollution
glow.** `accumCity` — accumulated for free along the existing atmosphere view-ray march using real
night-lights texture data — composites additively: `color += accumCity * vec3(1.0,0.72,0.42) *
nightFactor * kNightGlowScale;` (`sat_sky.frag:1579`). Reflect-Orbital already has an analogous,
fully-computed-but-unused-for-this brightness source: `beamGlowDomeBuf` (the 16-sector dome from
follow-up #31), which until now only SUPPRESSED other sky objects (Milky Way, satellites, stars) —
never added its own visible glow. That gap was the missing piece, and it needed zero new
computation (already bound in `sat_sky.frag` at binding 19).

**Changes:**
- `cloud_march.comp`: deleted the ENTIRE near-field bleed/march block (both #30's original and
  #38's march replacement) — nothing takes its place in this file. The tube's own `beamRadiance +=`
  (already multiplied by `crossfade`) is now the only per-beam sky contribution; it already fades
  smoothly to nothing as the observer approaches, which is now the whole near-field behavior.
- `sat_sky.frag`: new composite line right after the city-glow line, same general
  sky-color-building stage — a third independent copy of the 16-sector interpolated lookup formula
  (same established "duplicate per consumer" convention already used twice for this exact kind of
  dome data), reading `beamGlowDome[]` and adding `vec3(1.0,0.95,0.9) * beamGlowVal *
  pc.beamGlowBleedGain * kBeamSkyGlowScale * nightFactor` to `color`. Reuses `nightFactor` (computed
  one line above for city glow) rather than re-deriving day/night gating — accepted first-pass
  simplification (doesn't distinguish the observer's own day/night from the beam target's).
- `pc.beamGlowBleedGain` (the existing "Beam glow bleed gain" slider — no new slider, no
  settings.json changes) moved from `CloudMarchPC` (no longer used there) to `SatDrawPC` (172 bytes,
  was 168 — `CloudMarchPC` correspondingly shrank to 152, was 156) — mirrors the existing
  `beamMaxRangeM`/`beamSkyGlowGain` dual-mirroring pattern already used between these two structs.
  Caught during the build: `sat_sky.frag`'s own GLSL `PC` block declaration needed the new field
  added explicitly too (each shader stage declares its own view into the shared push-constant
  range) — a build error on the first attempt caught this immediately.
- Extension bug (#37, "only visible from above") explicitly NOT touched — user characterized it as
  "a tweakable fix," not broken; out of scope for this round.

Builds clean (both shaders recompiled, `.spv` timestamps confirmed newer than sources; C++
compiled/linked with no errors). **Not yet seen in-app** — expect standing inside/near a beam to
show the tube fading to nothing (no march artifacts, no added cost there) while the surrounding sky
reads as genuinely brighter; the satellite's own flare is unchanged; moving away should show the
new sky glow fading with the dome's existing distance falloff while the tube fades back in.

**Follow-up #40 (2026-07-27) — user report: neither the wash nor the fade were visible in-app; a
real units-mismatch bug in #39, plus exposing the fade radius as a slider.**

User tested #39 and reported seeing neither the sky-illumination wash nor any visible fade near a
beam. Root-caused two separate things, only one of which was an actual bug:

- **Real bug: `kBeamSkyGlowScale` (0.000002) was copied from `kNightGlowScale` (0.0000002) on the
  mistaken assumption they scale comparable quantities — they don't.** `kNightGlowScale` multiplies
  `accumCity`, a raw, UNNORMALIZED raymarch accumulation (tens of atmosphere samples of night-lights
  texture data summed together, easily reaching double digits), so it needs a tiny multiplier to
  land in a sane additive range. `beamGlowVal` (the new wash's input) is already Reinhard-compressed
  and `clamp`ed to `[0,1]` in `sat_orbit.comp` (follow-up #31/#32's dome-brightness formula) — an
  entirely different regime. Multiplying an already-normalized-to-1 value by a further 2e-6 crushed
  the wash's theoretical maximum to `1.0 (beamGlowVal) × 0.3 (default beamGlowBleedGain) × 2e-6
  (kBeamSkyGlowScale) ≈ 6e-7` — genuinely invisible against anything, not just under-tuned. Fixed by
  raising `kBeamSkyGlowScale` to `1.0` in `sat_sky.frag`, restoring the existing "Beam glow bleed
  gain" slider (0-2 range, default 0.3) as the real, visible control.
- **Also found while investigating: `cloud_march.comp`'s own GLSL `PC` struct declaration still had
  the OLD `beamGlowBleedGain` field from before #39 moved it out** — the C++ `CloudMarchPC` struct
  had already shrunk to 152 bytes, but this shader's own push-constant block still declared 156
  bytes' worth of fields (a stale leftover, not caught by the build since nothing in this file read
  that trailing field anymore — the same "each shader stage declares its own view into the shared
  push-constant range" gotcha #39 itself had already caught once for `sat_sky.frag`, just missed on
  the source-file side this time). Not the cause of the reported symptom (the stale field was inert),
  but real drift between the C++ and GLSL declarations — cleaned up as part of this pass by reusing
  the freed offset for the new field below rather than leaving it dangling.
- **The near-field fade itself (`crossfade`, unchanged since #30/#31) was never actually broken** —
  it already smoothly zeroes the tube's contribution within `kNearFieldCrossoverM` (a hardcoded
  15000m) of a beam's own 3D line. The user's report is read as: with the wash invisible (bug above)
  there was nothing to visually contrast against the vanishing tube, so the fade itself didn't read
  as an effect happening — combined with the explicit ask ("what variables control these, can they
  be made sliders"), exposed the radius as a tunable instead of leaving it a silent constant.

**Changes:**
- `sat_sky.frag`: `kBeamSkyGlowScale` raised `0.000002` → `1.0`, with a comment recording the
  units-mismatch reasoning above (so it isn't miscopied from `kNightGlowScale`'s neighborhood again).
- `cloud_march.comp`: removed the stale `beamGlowBleedGain` field from the shader's own `PC` block;
  added `beamNearFieldFadeM` in its place (reused offset, struct stays self-consistent with the new
  156-byte C++ size below). The local `kNearFieldCrossoverM` constant now reads `pc.beamNearFieldFadeM`
  instead of a hardcoded `15000.0`.
- `SatelliteSim.h`: `CloudMarchPC` grew 152→156 bytes for `beamNearFieldFadeM` (offset 152, the same
  slot `beamGlowBleedGain` vacated in #39). New CPU member `beamNearFieldFadeM = 15000.0f` (matches
  the old hardcoded default exactly, so behavior is unchanged until the user moves the slider).
  `hovCloudMinus`/`hovCloudPlus`/`draggingCloud` grew 46→47 for the new slider slot (idx 46).
- `SatelliteSim.cpp`: `recordCompute()`'s `CloudMarchPC` fill gained
  `cpc.beamNearFieldFadeM = beamNearFieldFadeM;`.
- `SatelliteSimUI.cpp`: new slider **"Beam near-field fade (m)"** (1000-50000, step 1000) added to
  the Terrain tab's beam slider group, idx 46, right after "Beam glow bleed gain". Persisted to
  `settings.json` as `beam_near_field_fade_m`.

Builds clean (`cmake --build build`, both shaders recompiled, `.spv` timestamps in
`build/Debug/shaders/` confirmed newer than sources; C++ compiled/linked with no errors). **Not yet
seen in-app.** Expect: the sky-illumination wash should now be clearly visible approaching an active
beam (tune via the existing "Beam glow bleed gain" slider, 0-2 range); the tube should visibly fade
out over the new "Beam near-field fade (m)" slider's radius (default still 15000m, matching prior
behavior) as a contrast against the now-visible wash, rather than fading into nothing-vs-nothing.

**Follow-up #41 (2026-07-27) — user tested #40 with the gain cranked up and found two real problems
with the wash itself: wrong distance measure, wrong shape.**

1. **Wrong distance measure.** The wash's brightness (`sat_orbit.comp`'s `domeVal`) falls off with
   `targetDist = length(targetENU)` — distance to the beam's GROUND TARGET only. Beams can be long;
   an observer climbing up alongside a beam toward the satellite end stays close to its actual LINE
   the whole way, but `targetDist` grows, so the wash incorrectly faded out despite genuine
   proximity. The exact same class of bug follow-up #31 already fixed once for the tube's own
   `crossfade` (point-to-*segment*, not point-to-*endpoint*) — the wash never got that fix when #39
   added it.
2. **Wrong shape.** The wash reused the Light Pollution Dome's exact idiom: 16 azimuth sectors ×
   horizon-brightest elevation falloff — right for a city's broad glow dome in one compass
   direction, wrong for one narrow, often low-angle beam. Read as a rising pillar, not a general
   brightening. **User's own proposed fix:** build a real distance-to-beam function, then just use it
   to amplify sky brightness uniformly — no azimuth/elevation dependence.

**Implementation reused and fixed existing infrastructure instead of adding new GPU plumbing.**
`SatelliteSim::recordCompute()` already reads back `reflectBeamsBuf` on the CPU every frame (a
one-frame-stale diagnostic, `lastActiveBeamCount`/`lastNearestBeamDistM`) — and it had the SAME
"distance to target, not to line" bug (`glm::length(entries[s].targetENU)`). `GpuReflectBeam`
entries already store both `satENU` and `targetENU` (observer-relative — origin is the observer), so
a proper point-to-segment distance was a small formula change to this existing loop, fixing the
Settings beam-debug readout's own latent inaccuracy as a side effect. No new buffer, no new
descriptor bindings, no new barriers — considered and rejected a GPU-side `atomicMin`-into-a-new-
buffer approach (fully researched: `orbitDescSet` binding 7 and `skyDescSet` binding 20 were both
free) in favor of this much smaller change, since the CPU already had the data and already threads
per-frame scalars into `sat_sky.frag` via `SatDrawPC`.

**Changes:**
- `SatelliteSim.cpp`: the `reflectBeamsBuf` readback loop (~line 439) now computes point-to-segment
  distance (`slantRangeM = length(satENU-targetENU)`, `dirUp = (satENU-targetENU)/slantRangeM`,
  `t = clamp(-dot(targetENU,dirUp), 0, slantRangeM)`, `d = length(targetENU + dirUp*t)`) instead of
  `length(targetENU)`. Right after, a new block computes `beamProximityGlow` — a ready-to-use [0,1]
  value, hand-rolled smoothstep of `lastNearestBeamDistM` over `beamNearFieldFadeM` (the SAME radius
  slider #40 added for the tube's own crossfade — reused here exactly as the user suggested, one
  distance function driving both fades). `buildSatDrawPC()` gained
  `pc.beamProximityGlow = beamProximityGlow;`.
- `SatelliteSim.h`: new CPU member `beamProximityGlow`; `SatDrawPC` grew 172→176 bytes for the
  mirrored field.
- `sat_sky.frag`: `PC` struct gained `beamProximityGlow`. The entire azimuth-sector-lookup wash
  block (`azBeam`/`secFBeam`/`sec0Beam`/`beamAzGlow`/`elevFalloffBeam`/`beamGlowVal`) deleted,
  replaced with a single non-directional composite:
  `color += vec3(1.0,0.95,0.9) * pc.beamProximityGlow * pc.beamGlowBleedGain * kBeamSkyGlowScale *
  nightFactor;` — no `dir`/azimuth/elevation dependence, so proximity brightens the whole visible
  sky uniformly. `beamGlowDomeBuf` itself untouched — still used for its original suppression
  purpose (Milky Way section here, `sat_flare.comp`, `updateStars()`).

Builds clean (`cmake --build build`, `sat_sky.frag.spv` timestamp confirmed newer than source;
`SatDrawPC` static_assert passes at 176 bytes; C++ compiled/linked with no errors). **Not yet seen
in-app.** Expect: climbing up alongside a long beam, well away from its ground target, should now
still show the sky brightening (previously faded out there); the brightening should read as a
uniform wash across the whole sky, not a narrow column in one direction; the Settings beam-debug
"nearest beam distance" readout should reflect true line distance now (a minor, expected side effect).

**Follow-up #42 (2026-07-27) — the beam extension (#36/#37) never actually rendered, plus real
terrain/ocean occlusion under-sampling and a precision mismatch, both fixed.**

1. **Extension geometry bug, found by re-reading the full per-beam block end-to-end.** The
   extension's position formula (`p` split at `tBeam=0`, using `horizontalDirUp` for the negative
   region) was correct since #37. The bug was upstream: the closest-approach solve that decides
   WHICH `tBeam` value (and therefore point) a given pixel gets evaluated at was still solved
   entirely against the tilted tube line (`rayDirUp`) — `tBeamRaw` answers "where along the TILTED
   line is this view ray closest," which has no relationship to "where along the HORIZONTAL ground
   extension is this view ray closest." A pixel looking at a point 5km past the target along the
   ground got assigned a `tBeamRaw` from the wrong line's geometry — usually far outside
   `[-extraLengthM, tTop]` — so either the resulting (clamped) point didn't match what the pixel was
   looking at (`perpDist` large → near-zero `proximity`), or `overshoot` (also built from the same
   wrong `tBeamRaw`) blew up and collapsed `endFade`. Both failure modes converge on the same visible
   symptom: the extension only ever lit up right at `t≈0` — i.e. exactly at the target, reading as
   "the beam terminates there" regardless of `extraLengthM`'s actual size.
   
   **Fix: two independent closest-approach solves, tube (`rayDirUp`, `t∈[0,tTop]`) and extension
   (`horizontalDirUp`, `t∈[0,extraLengthM]`), each producing its own point/`perpDist`/`overshoot`/
   `sinTheta` — the branch with the SMALLER `perpDist` wins** and its values (`p`, `sView`,
   `perpDist`, `overshoot`, `sinTheta`, signed `tBeam`) feed all the existing downstream formulas
   unchanged (`distFromSatM`, `tubeRadiusM`, `proximity`, `grazingLen`, `terrainVis`, the final
   `beamRadiance +=`). This falls out correctly per-pixel with no explicit "which side of the target"
   test: a pixel looking at the ground extension is naturally closer to that line, a pixel looking up
   at the tube is naturally closer to that one. Both branches agree exactly at their shared `t=0`
   boundary, so there's no seam. `r`/`c` (origin-independent of line direction) are shared between
   both solves; only `bDot`/`denom`/`f`/the raw-t formula differ per branch. Also fixed in passing:
   `sinThetaClamped`'s divisor was always `tubeRadiusM/tTop` (the TUBE's length) even for the
   extension branch, over-amplifying its grazing-angle boost — now `tubeRadiusM/segLen`, where
   `segLen` is each branch's own natural length cap (`tTop` or `extraLengthM`).
   
   Considered and rejected: falling back to fixed multi-sample marching along just the short
   extension (mirroring the debug-ray section's `kDebugRaySamples` pattern) — rejected to stay
   consistent with this feature's established analytic, O(1)-per-beam architecture (the reason
   follow-up #23 replaced an earlier 32-sample march in the first place), and because the two-branch
   solve is the more principled fix, not an approximation.

2. **Terrain/ocean occlusion (`beamTerrainVisibility()`) had two independent real bugs**, found by
   direct comparison against `sat_sky.frag`'s own (already-adaptive, session 29) terrain march:
   - **Fixed sample count (8), regardless of line length.** For a beam hundreds of km from the
     observer, 8 samples land tens of km apart — any hill between two samples was completely
     undetected, not just soft-edged. Fixed with the same path-length-adaptive pattern
     `sat_sky.frag`'s own terrain march already uses (`sat_sky.frag:1338-1374`, confirmed via
     research this round): `kBeamOcclusionSteps = clamp(int(lineLen/3000.0), 8, 32)` — 8 as the floor
     (no regression for the common near-field case), 32 as a cost-bounded ceiling (this runs
     per-beam per-pixel, already thinned at range by the existing `kBeamGlowStride` mechanism).
   - **`terrainHeightAtDir()` sampled at mip 2.0** while `sat_sky.frag`'s own terrain
     march/bisection/normal computation all sample the SAME textures at mip 0.0 (confirmed via
     research: `sat_sky.frag:1312-1417`) — a real, undocumented precision mismatch: the beam
     occlusion test was checking against a blurrier version of the terrain than what's actually
     drawn, letting beams clip through real hills the test couldn't "see," or get needlessly dimmed
     by a mip-2 bump that doesn't exist in the mip-0 terrain the camera renders. Changed both
     `textureLod` calls in `terrainHeightAtDir()` to mip 0.0.

Builds clean (`cmake --build build`, `cloud_march.comp.spv` timestamp confirmed newer than source; no
other files touched this round). **Not yet seen in-app.** Expect: low-elevation beams should now
visibly continue past their target, connecting to the stretched ground ellipse instead of stopping
dead at the target point; beam/terrain/ocean interaction artifacts (beams clipping through hills, or
dimming in open air near terrain) should be visibly reduced, especially for beams far from the
observer or near rugged terrain.

**Follow-up #43 (2026-07-27) — reverted #35's elliptical ground footprint back to isotropic circular,
per user report.** After #42's extension fix made the true sky-to-ground beam geometry more visible,
the user traced actual beam impact points in-app and found the ellipse's stretch axis/magnitude
(derived from that one satellite's own beam angle) didn't match where the light visibly appeared to
land — explicitly attributed to the ellipse being "based on only 1 satellite beam angle." Reverted
`sat_sky.frag`'s ground-spot term to a plain isotropic Gaussian (`groundDist = length(hitPt.xy -
targetENU.xy)`, `footprint`/`core` as a function of `groundDist` alone) — `footprintR`/`coreR`
themselves are unchanged (still #34's physically-derived minor-axis-equivalent radii); only this
term's own consumption reverts. `elevFade` (the 5°-cutoff matching the sky tube's own gate) was KEPT
even though its original motivating concern (the ellipse stretching to infinity as `sinElev→0`) no
longer applies — independently reasonable for consistency with the sky portion vanishing at the same
elevation. Builds clean, `sat_sky.frag.spv` timestamp confirmed refreshed. **Not yet seen in-app.**

Also raised this same round, not yet acted on — see the memory file `project_c12_reflect_beams.md`
for the fuller writeup:
- **"Do we compute depth when we draw our beams?"** Answered directly: no true per-pixel depth-sorted
  compositing exists for beams. `cloud_march.comp` writes ONE shared scalar, `tEnterCombined` (the
  nearest entry distance among ALL cirrus/cloud/beam/aurora content combined, at HALF resolution);
  `sat_sky.frag` compares that single value against this pixel's real (full-res, exact) terrain hit
  distance `tSurface` and, if terrain is nearer, suppresses the ENTIRE combined buffer for that pixel
  — all types at once, no per-layer occlusion. This means a beam that's actually behind a hill can
  still render if some unrelated CLOSER cloud/cirrus content happens to be nearer than that hill on
  the same ray (nothing keeps the layers' occlusion independent), on top of the half-resolution
  sampling itself producing blockier suppression boundaries than the full-res terrain silhouette
  they're being compared against. Separately, `beamTerrainVisibility()` (improved in #42) is its own
  approximate line-of-sight march, not a true depth-buffer test either.
- **Recommendation offered, not yet implemented or agreed to:** give beams their own accurate
  occlusion test directly in `sat_sky.frag`, using the EXACT `tSurface` already computed for that
  exact pixel, instead of relying on the shared half-res composite's single scalar — beams are a
  bounded/sparse count (unlike full volumetric clouds), so this is likely affordable, and would fix
  both the cross-layer occlusion bug and the half-res blockiness. Tradeoff: moves (or duplicates) beam
  glow evaluation into the full-res fragment shader — more per-pixel work, and code duplication
  between the two shaders, though that's already an established, accepted pattern in this file
  (cirrus/cloud march functions are already hand-duplicated between `cloud_march.comp` and
  `sat_sky.frag` per `[[feedback_shared_ubo_duplication]]`-style convention).
- **"Does the beam glow from being within beams just flatly illuminate the entire screen?"** Confirmed
  yes — `pc.beamProximityGlow` (#41) is one CPU-computed scalar per frame, added uniformly to every
  SKY pixel's color with zero directional/positional variation (does not touch terrain/ocean/UI, only
  the atmospheric sky backdrop). This is exactly what was requested in #41 ("just amplify the sky
  brightness when inside a beam"), so not a bug — flagged back to the user to confirm this flat/
  uniform feel is still what's wanted, or whether some (correctly-implemented, non-pillar) soft
  directionality should be reintroduced.

### 2026-07-20/21 (session 31) — Cloud shape quality pass + domain-warp bake perf
Three user-reported cloud oddities (flat bases, conical/not-fluffy shapes, shadow line artifacts
+ phantom thin-cloud shadow-casters), followed by a perf regression from fixing the third one, then
two rounds fixing a new artifact class the perf fix introduced. All changes in `cloudDensity()`/
`cloudMarchCS()`/`cloudWarpOffset()` in `cloud_march.comp` (the single source of truth for cloud
density since session 23 — see that entry) plus a new bake shader.

- **Flat bases fixed:** the vertical base cutoff was a hardcoded `smoothstep(0.0, 0.05, hNorm)` —
  literally the same altitude for every cloud on Earth, no noise input at all (the cloud *top*
  already varied per-column via the existing `colH` noise, which is why only the base looked
  artificially flat). Added a noise-driven `baseH` (same warped column coordinate as `colH`,
  decorrelated Z offset), scaled by a new **"Base variance"** slider (default 0.3, 0 = old flat
  behavior). Applied identically in both the main march's `hFade` and the self-shadow cone's
  `chFade` for consistency.
- **Conical/smooth shapes fixed:** root cause was `base = mix(baseA, topA, hNorm)` — a straight
  lerp between exactly TWO fixed noise Z-slices, which is geometrically a cone by construction (no
  real mid-height structure to deviate from a taper). Added a third mid-height presence slice
  (`kPresenceZMid=0.28`) — still discrete fixed Z-slices, not continuous Z sampling, to avoid
  reintroducing the altitude-slab banding bug from [[project_cloud_march_steps — 150 march steps
  minimum]] (session pre-23). Also made erosion edge-biased: `erosionAmt = mix(cloudErosionEdge,
  cloudErosionCore, base)` (new sliders, defaults 0.5/0.15) so edges fray while dense cores stay
  comparatively solid, instead of one uniform erosion strength that just read as smooth downscaling.
- **Shadow line artifacts + phantom shadow-casters fixed:** the self-shadow light cone sampled a
  FIXED `(ci+0.5)*coneSeg` phase along `sunDir` through structured Worley noise — classic aliasing,
  read as coherent parallel-line banding. Added per-pixel jitter (`noiseTex`-based, same pattern
  `cirrusMarchCS` already used for its own step jitter). Separately, the cone's `chFade` ignored
  the per-column `colH`/`baseH` entirely, treating the whole 5%-95% of the shell as "possible
  cloud" regardless of how little of that column the visible march actually filled — so a
  near-clear or single-march-step-thin column (visible in `CLOUD_DEBUG 2`) could still cast a
  full-height shadow with no matching visible cloud. Cone now computes its own `colH`/`baseH` per
  sample, using the same warped coordinate the visible density lookup uses.
- **`GpuCloudParams` UBO: no struct growth.** All three new tunables above repurposed the
  already-reserved `pad4`/`pad5`/`pad6` slots (`cloudBaseVariance`/`cloudErosionEdge`/
  `cloudErosionCore`). Mirrored (layout-parity only) in `sat_sky.frag`'s duplicate struct copy —
  see the shared-UBO-duplication rule noted in session 24's entry.
- **Bug: settings window broke after adding the new sliders.** `draggingCloud[33]` (one of three
  parallel arrays keyed by `CloudSlider` `idx`, alongside `hovCloudMinus`/`hovCloudPlus`) was never
  resized when the sliders above were added — writes for the new ids landed out of bounds and
  corrupted the window-chrome state declared immediately after it in the class, so the settings
  window closed itself instantly and sliders read as permanently "stuck dragging." (`draggingCloud`
  had actually been one short since an EARLIER session's "Night ambient" slider addition — the hover
  arrays were resized then but this one was missed.) Fixed by resizing all three arrays together
  (37, later 38 once the sun-gain-zenith slider below was added); no bounds-checked container
  would have caught this at compile time, so any future slider addition to this shared tab system
  must check all three by hand.
- **Sun gain no longer flat across all sun elevations.** `cloud.sunGain` was one multiplier at
  every elevation, so a value tuned to look amazing at sunset (5.0) overexposed clouds at midday.
  Split into `cloud.sunGain` (near-horizon/sunset endpoint) and new `cloud.sunGainZenith` (midday
  endpoint, default 1.0), blended by `cloudSunDotRaw` (≈sin(sun elevation)) in `cloudMarchCS`,
  `cirrusMarchCS`, and `evalCloudLayer` (the orbit-altitude flat-cloud fallback in `sat_sky.frag`).
  Repurposed the free `pad3` slot — still no struct growth. Settings: "Sun gain (horizon)" /
  "Sun gain (zenith)", Clouds tab.
- **Perf regression from the shadow-cone fix above, then fixed:** making the cone warp-consistent
  with the visible cloud (`cUVWXY` needing the same `cloudWarpOffset` the main density lookup uses)
  meant calling `cloudWarpOffset` (3 live `warpPerlin3` evaluations, 24 gradient-hash lookups) fresh
  at every cone step — new cost that didn't exist before. Fixed for free by reusing the PARENT
  march sample's already-computed `warpUVW` instead of recomputing it per cone-step position (cone
  samples sit within ~2×shellThick of the parent, a few km, negligible against the warp's own low
  frequency `kWarpFreq=6`).
- **New profiling toggle:** added a 7th KNOCKOUT PROFILING checkbox, "Cloud self-shadow cone"
  (`debugDisableMask` bit 64, checked directly in `cloudMarchCS`), so this block's isolated GPU
  cost can be read off the existing GPU FRAME BREAKDOWN without a capture tool — mirrors the six
  toggles session 29 built.
- **Domain-warp baked into a new 3D texture, replacing live `warpPerlin3` in `cloudWarpOffset`**
  (`cloudWarpOffset` runs once per in-cloud march sample — the dominant remaining live-procedural
  cost). New `shaders/cloud_warp_noise.comp`, same one-shot-bake pattern as `cloud_noise.comp`/
  `aurora_noise.comp`, new `createCloudWarpNoisePipeline()` in `SatelliteSim.cpp`, new binding 9 on
  `cloud_march.comp`'s descriptor set. **Deliberate tiling trade-off:** the live version was
  genuinely unbounded, non-tiling Perlin noise (never repeated) — baking it necessarily makes it
  periodic at some cell count. Chose `kWarpBakeN=16` specifically so the full visible sky dome only
  covers ~3/4 of one tile — no repetition visible within a session at default drift rates; this is
  the same mechanism [[project_cloud_next_session]] flagged as the intended fix for the cloud
  DENSITY noise's own large-scale tiling problem, so baking it was a real (if distant) risk against
  that goal, not a free win — documented at the top of `cloud_warp_noise.comp` for future reference.
  - **Round 1 (128³, single Perlin octave, 8 texels/cell): visible "tessellating triangle"
    faceting**, reported immediately. This was NOT the tiling/domain-seam bug class from
    [[project_cloud_seam_bug]] (already resolved, non-power-of-2 N) — root cause was
    interpolation METHOD. Hardware trilinear filtering between baked texels is LINEAR
    (straight-edged); the live analytic version was smoothstep-interpolated (curved) between its
    gradient corners. At sparse texel density with only one Perlin octave, that mismatch was
    geometrically legible once the value was used as a raw coordinate displacement — worse than
    in `cloud_noise.comp`, whose comparable R channel sums 3 octaves and gets thresholded into a
    coverage mask rather than used as a raw offset.
  - **Round 2 (192³, two-octave FBM — matched `cloud_noise.comp`'s own proven resolution/N
    choices exactly): reduced but did not eliminate** the faceting (user: "finer" but still
    visible) — confirmed texel density/octave count were contributing factors but not the root
    cause.
  - **Round 3 (final fix): replaced hardware trilinear with a manual smoothstep-weighted blend.**
    `cloudWarpOffset` no longer calls `texture()`; a new `cloudWarpNoiseSample()` helper does its
    own 8-corner `texelFetch` + `mix()` using the SAME `f*f*(3-2*f)` weight curve the original
    analytic evaluation used at its gradient corners, with manual wraparound (`wrapTexel`/
    `wrapTexel3`, same explicit `if(r<0) r+=n` pattern as `gradHash` — not GLSL's `%`, whose
    negative-operand behavior isn't safe to rely on across drivers). The C++-side sampler's
    REPEAT/LINEAR settings are now vestigial for this texture (texelFetch bypasses both). 8
    `texelFetch` calls vs. 1 `texture()` read, still far cheaper than the original 3× live
    `warpPerlin3`. **User-confirmed: "the perfect effect, and performance is great."**
- **Perf data caveat:** `perf_profiles/profile_log.jsonl` snapshots from this session
  (`2026-07-21` group, `cloud_march` 4-21ms depending heavily on camera altitude/angle/coverage)
  don't cleanly isolate this session's wins in isolation — no build-version tag on snapshots, no
  paired before/after captures at matched camera state, and the new bit-64 shadow-cone toggle
  wasn't exercised in any saved snapshot. Scene variance dominates snapshot-to-snapshot cost more
  than implementation differences at this sample size (consistent with session 29's own
  correlation-table caveat "small sample, treat as directional"). Final state is verified
  qualitatively (user confirmed both the visual fix and that performance recovered), not via a
  rigorous quantified A/B from this log.

### 2026-07-17 (session 29) — GPU performance profiling infrastructure + aurora/terrain/airglow optimization
- **New: in-app GPU timestamp profiling.** `VulkanContext` gained a 7-slot `VK_QUERY_TYPE_TIMESTAMP`
  query pool (single frame in flight, so results are resolved right after the fence wait in
  `App::drawFrame` with no stall). Slot layout is a shared contract: App.cpp writes 0 (frame
  start), 5 (satellite+star draw done), 6 (UI overlay done); `SatelliteSim` writes 1-3 in
  `recordCompute` (cloud march / orbit compute / flare compute) and 4 in `recordDraw` (sky
  background draw done) — this SPLIT what used to be one fused "sky_terrain_draw" bucket into
  `sky_background_draw` (just the fullscreen atmosphere/terrain/ocean shader) and
  `satellite_star_draw` (the point sprite passes), which turned out to be ~0.05ms — negligible even
  with 1M+ active satellites, resolving an earlier open question about whether satellite draw cost
  was a factor (it never was). `SatelliteSim::updateGpuTimingStats()` EMA-smooths the six deltas
  into `gpuMsSmoothed[6]`, displayed in a new "GPU FRAME BREAKDOWN" section on the Display settings
  tab, one-frame-stale by construction (same pattern as `peakMagnitude`).
- **New: perf knockout toggles.** `SatDrawPC` grew 128→132 bytes for `debugDisableMask` (a
  profiling-only bitmask consumed by `sat_sky.frag`'s `dbgSkip*()` helpers), later mirrored into
  `CloudMarchPC` (128→132 bytes) once parts of the shader moved to `cloud_march.comp` mid-session.
  Six checkboxes in the Display tab ("KNOCKOUT PROFILING") let each toggle disable one block —
  terrain march, atmosphere loop, sun optical depth (`optDepth`, called from 4 sites), ocean sky
  reflection, airglow red, aurora — with a mathematically-safe zero/no-op fallback per toggle (e.g.
  terrain-skip leaves `tHit=-1`, the same value the "no hit" path already produces normally).
  Default mask 0 is bit-identical to normal rendering.
- **New: `perf_profiles/profile_log.jsonl` + "Save Snapshot" button.** Appends one JSON record
  (GPU timing breakdown, resolution, observer lat/lon/altitude, sim time, active knockout mask, GPU
  device name, quality settings) per button press — JSON Lines so the log grows by simple
  appending across sessions. `SatelliteSim::savePerfSnapshot()`.
- **New: `tools/perf_analysis/` Python toolkit** (gitignored `.venv`, `requirements.txt`: pandas +
  matplotlib). `analyze_profile.py` reports GPU cost by resolution bucket, per-megapixel
  normalization (tests whether a pass is purely resolution-bound), a matched-altitude resolution
  ratio (isolates the resolution effect from confounding altitude/scene changes in the same
  dataset), a knockout-toggle cost summary, and Pearson correlations — plus two PNG plots.
- **Terrain march: fixed a real bug, not just a perf tweak.** Step count (`kN`) was purely a
  function of OBSERVER ALTITUDE (`mix(196, 320, obsEffH/800000)`) — a grazing/horizon ray and a
  steep near-vertical ray from the SAME ground-level observer got the identical step budget,
  despite the grazing ray covering far more physical distance and needing proportionally more
  steps to avoid undersampling (this was very likely a real contributor to reported terrain
  jitter, not just wasted cost). Changed to scale off this ray's own `tExit` instead, working out
  the quadratic-spacing math (`dt ≈ 2*(tExit-2)/kN` at the coarse far end) to hit the same
  historical "~2.8km coarsest step" calibration. Range re-tuned by the user in-app from the
  original 196-320 down to 64-164 (jittery-but-passable at the 64 floor). User confirmed: at a
  near-terrain test spot, disabling terrain via the knockout toggle now costs ~1fps, down from
  being one of the largest individual `sky_background_draw` contributors.
- **Aurora: went from the single dominant cost (up to ~44fps swing toggled on/off) to a minor one,
  across four rounds — user's own framing, "a full march across the planet for a very sparse
  effect", drove the last two:**
  1. Step cap `kAuroraStepsMax` 160→64 (`kAuroraStepsMin` 24→4, user-tested — briefly pinned to a
     flat 4/4 to find the visual floor, which banded badly at grazing angles, then raised until
     acceptable).
  2. Two cheap pre-filters: a per-SAMPLE conservative colatitude bound inside `auroraOvalMask`
     (skips its 2 `warpPerlin3` calls — ripple + coverage — for any sample farther than the
     provably-unreachable 64° worst-case oval reach, derived from `stormStrength`'s slider-enforced
     [0,1] range), and a per-RAY whole-march bounding pre-check (5 cheap position-only colatitude
     samples spread across the ray's shell-crossing span; skips the ENTIRE march, not just
     individual samples, for rays that never come near the oval at all — the direct fix for "most
     of the sky, most of the time, even when standing in an aurora-active region").
  3. **Baked 3 of the remaining ~6-7 live analytic `warpPerlin3` calls per sample into a texture**
     (`aurora_noise.comp`, 1024×16×256 RGBA8, `createAuroraNoisePipeline` — same one-shot-bake-at-
     init pattern as `cloud_noise.comp`/`createCloudNoisePipeline`). R = curtain fold `base`
     (dropped the second "detail" octave first, then baked the remaining one); G/B = column-window
     `colA`/`colB`. This is the direct answer to "why is aurora more expensive than clouds despite
     looking simpler": clouds' noise was already moved off the live shading path into a texture in
     a prior session (`cloud_noise.comp`); aurora's never was until now. U (azimuth) wraps — MUST
     be power-of-2 cell count for the tiling hash to avoid a real seam (same lesson
     `CLOUD_SEAM_BUG.md` already paid for) — 256 cells/loop for the curtain (vs. the live
     `kAuroraTangentFreq(40)*2π≈251.3`), 64 for the column window (vs. `kAuroraColumnFreq(9)*2π≈
     56.5`). V (altitude, curtain only) and W (colatitude, both) don't wrap — sampled with
     CLAMP_TO_EDGE, never read across their own bake-volume edge. `shimmerPhase` (the animation
     driver) stays live and cheap (1 call), offsetting the SAMPLE COORDINATE into the static baked
     texture — same "warp the lookup, bake the detail" split `cloudWarpOffset` already uses.
     Accepted approximations, flagged for the user to specifically check visually: fold
     spacing/density is a power-of-2 approximation of the true frequency (small, ~2-13% drift);
     `stormStrength`'s "more folds" effect now comes from wrapping the baked pattern more times
     rather than genuinely new higher-frequency detail; column-window decorrelation now comes from
     an additive coordinate offset instead of a true third noise axis. User tested and found no
     meaningful visual difference.
  4. **Moved the whole sky curtain march into `cloud_march.comp`**, running at half resolution
     (1/4 pixels) in the same dispatch as clouds/cirrus instead of full-res in `sat_sky.frag` — the
     single biggest win of the four, user-confirmed "night and day difference" via before/after
     snapshots (deltas dropped from the 9-13ms range to 1-2.5ms at comparable locations). New
     `auroraMarchCS`/`auroraCurtainSample`/`auroraOvalMask`/`auroraCoverage`/`auroraFrame` in
     `cloud_march.comp` (near-verbatim ports); result folds additively into the same `B_total`
     channel clouds already write, so `sat_sky.frag` needed no new sampling code at all — aurora
     just rides along inside the cloud composite it already reads. Cloud-suppression
     (`auroraBehindClouds`/cubed-transmittance-when-behind) now uses the LOCAL `A_total` computed
     in the same shader invocation instead of the old cross-resolution texture pre-sample —
     strictly more accurate, not just faster. `sat_sky.frag` keeps its own copies of
     `auroraFrame`/`auroraCoverage`/`auroraOvalMask`/`auroraCurtainNoise`/`auroraSampleAt` — still
     used by `auroraGlowAt` (terrain/ocean ambient) and the ocean sky-reflection's own aurora
     sample, both legitimately full-resolution/terrain-coupled.
  - **Real behavior change, not just perf:** aurora (and the airglow-red band, see below) now
    respect terrain occlusion the same way clouds do, since they're folded into the same
    terrain-gated composite branch in `sat_sky.frag` (`if (!(tSurface>0 && tEnterCombined>=0 &&
    tSurface<tEnterCombined))`). Previously aurora ignored terrain entirely — visible "through" a
    mountain that should have blocked the view. Judged more physically correct (terrain close
    enough to block the much-nearer cloud layer necessarily blocks the far-higher aurora shell
    along that same ray too) and accepted without further tuning.
- **Airglow red band also moved to `cloud_march.comp`** (`airglowRedMarchCS`), same half-res
  pattern, no cloud-suppression term (the original never had one). Much smaller win going in
  (~1ms full-res, fixed 16 steps × 1 noise call/sample — nowhere near aurora's old worst case) but
  free given the aurora infrastructure already existed. Green/sodium airglow bands stay in
  `sat_sky.frag`'s main atmosphere loop (they ride its samples "for free," no separate march to
  move) — moving just them would mean duplicating a chunk of that loop for near-zero marginal win.
- **Corrected a stale assumption from session 24:** a transmittance LUT (replacing `optDepth`'s
  inner march) was flagged there as "the natural next step" for perf, based on it being the
  obvious textbook target for a nested O(N_VIEW×N_LIGHT) loop. This session's knockout-toggle data
  showed the opposite: `optDepth`'s isolated cost was consistently near-zero (0.1-1.4ms deltas)
  across every test location — terrain and aurora were the actual dominant costs, never this.
  Treat that session-24 guidance as superseded; don't rebuild the LUT on the strength of the old
  note without fresh data motivating it.
- **Why the main atmosphere loop (N_VIEW, in `sat_sky.frag`) is NOT a similar half-res-move
  candidate, unlike clouds/cirrus/aurora/airglow-red — two concrete reasons, not just caution:**
  its endpoint is terrain-distance-coupled (`tEnd = min(tAtmos.y, tSurface)`, truncated at the
  exact per-pixel terrain hit for correct aerial-perspective haze — the shell marches all
  explicitly avoid this exact coupling, see `cloud_march.comp`'s own header comment on having "NO
  terrain data"), and its accumulated `odR_cam`/`odM_cam` directly attenuates the satellite
  glow/flare aggregate render (`sat_sky.frag` ~line 1660) — the literal content-fidelity concern
  that ruled out whole-frame resolution scaling earlier this session. A move here would need to
  split "pure sky gradient" (resolution-insensitive) from "terrain-exact haze + satellite
  attenuation" (needs per-pixel precision) — a materially bigger, riskier redesign than anything
  done this session. Not started; flag for a future session if pursued.
- **Resolution scaling — implemented and shipped, same session.** Settings > Display > "Render
  scale" (50%-100%, default 100%, moved to the top of the tab — a real user-facing option, not a
  debug tool, so it doesn't live down by the knockout toggles). Only the sky/terrain/ocean/cloud-
  composite background scales; satellites, stars, and UI always render at native resolution — the
  design goal from the start, given the earlier-session concern about losing tiny satellite point
  fidelity to a whole-frame downscale.
  - **Safety-first architecture, since this can't be tested by the assistant directly:** at the
    default `renderScale==1.0`, the code path is BYTE-IDENTICAL to before this feature existed —
    `sat_sky.frag` draws inline in `recordDraw`'s existing Pass 1 exactly as always. Only when the
    user actively drops below 100% does a new path activate: the background renders into a low-res
    offscreen target (`skyLowResColorImg`, its own single-color-attachment render pass
    `skyLowResRenderPass`, CLEAR each frame, `finalLayout=TRANSFER_SRC_OPTIMAL`), then gets
    linearly upscaled via `vkCmdBlitImage` directly into the swapchain image BEFORE the main
    render pass opens (`Simulation::recordPrePass`, a new hook — default no-op, so
    GameOfLife/Particles/Scene3DDemo needed zero changes).
  - **The load/clear render-pass problem:** the main render pass can't simply switch its color
    attachment's loadOp from CLEAR to LOAD per-frame (baked into the VkRenderPass object at
    creation). Solution: a second render pass object, `ctx.renderPassLoad` — identical attachment
    formats/sample-counts to `ctx.renderPass` (so it stays compatible with the SAME
    `ctx.framebuffers`, per Vulkan's render-pass-compatibility rules, which only check format/
    samples, not load/store ops or layouts) but LOAD instead of CLEAR for color, with
    `initialLayout=TRANSFER_DST_OPTIMAL` matching what the pre-pass blit leaves the swapchain
    image in. `Simulation::activeRenderPass(ctx)` — another new hook, default returns
    `ctx.renderPass` unchanged — lets `SatelliteSim` pick `ctx.renderPassLoad` only when scaled.
  - **Depth is deliberately NOT blitted** — a considered, documented tradeoff, not an oversight.
    Reasoning at the time: depth-format blit support isn't spec-guaranteed the way color-format
    blit support effectively always is, and that gap is more likely on exactly the lower-end
    hardware this feature targets; the assistant couldn't verify it either way without running the
    app. Consequence: satellites/stars are not occluded by terrain while `renderScale<1.0`
    specifically (a satellite that should hide behind a mountain may show through) — confirmed
    acceptable, not revisited.
  - **Real bug found and fixed same session: cloud composite drifted off-center as renderScale
    dropped, fully distorted at 50%.** Root cause: `sat_sky.frag`'s cloud-target UV sample
    (`gl_FragCoord.xy / (textureSize(cloudTargetA,0)*2.0)`) silently assumed `gl_FragCoord` always
    spans the FULL swap extent — true before this feature existed, false once the background can
    render into a smaller offscreen framebuffer (`cloud_march.comp`'s own dispatch is unaffected
    by `renderScale`, always sized off the true swap extent, so the mismatch grows with scale).
    Fixed by adding `screenSizePx` to `SatDrawPC` (128->144 bytes this time — needed an explicit
    `pad0` float first, since GLSL's push_constant block requires 8-byte alignment for the `vec2`
    that C++ doesn't insert automatically) carrying THIS draw's actual target size
    (`skyLowResExtent` when pre-rendering the scaled background, `ctx.swapExtent` everywhere else)
    — `cloudUV` now divides by that instead of the stale assumption. **Any future `gl_FragCoord`-
    based UV/lookup math in `sat_sky.frag` must use `pc.screenSizePx`, not an assumed full-res
    constant, or it will silently break the same way the moment resolution scaling is active** —
    the terrain-march jitter lookup (`gl_FragCoord.xy * (1.0/128.0)`) was checked and is fine,
    since it's a fixed-frequency noise seed with no total-resolution assumption baked in, not a
    [0,1]-normalized UV.
  - `buildSatDrawPC(ctx, targetExtent)` factors out the push-constant fill shared by
    `recordPrePass` and `recordDraw`, parameterized on target extent specifically so `aspect`
    (always the true screen aspect — the camera's real FOV never changes just because the sky pass
    rendered smaller) and `screenSizePx` (always THIS draw's actual target) can't drift apart.
  - `App.cpp`'s submit wait stage grew from `COLOR_ATTACHMENT_OUTPUT_BIT` alone to also include
    `TRANSFER_BIT` — the pre-pass blit writes the swapchain image at the transfer stage, which the
    old wait mask didn't gate, a real (if narrow) race against the presentation engine.

### 2026-07-15 (session 28)
- **C16 — Aurora, implemented.** New "curtain primitive" in `sat_sky.frag`: `auroraFrame()`
  computes colatitude/azimuth (+ radial/tangent basis) of a point relative to whichever
  geomagnetic pole it's nearer (`kGeomagPoleECEF = vec3(0.0481, -0.1543, 0.9868)`, 80.7°N/72.7°W;
  south = negate, since geomagnetic poles are antipodal under a dipole model — one constant
  covers both hemispheres). `auroraOvalMask()` is a colatitude band around `kAuroraOvalColatDeg`
  whose centerline is ripple-displaced by azimuth+time (`warpPerlin3` sampled on `(cos az, sin
  az)` to avoid a seam at az=±π — same trick `cloudWarpOffset` uses for its own seam-avoidance).
  `auroraCurtainNoise()` is the part that actually sells it as aurora rather than a flat glow band:
  two `warpPerlin3` samples with DIFFERENT frequencies on the tangent (azimuthal, high freq → many
  separate folds) vs. radial (colatitude, low freq → each fold reads as a long unbroken streak
  toward the pole, not a blob) axes — the same anisotropic-stretch idea cirrus streaks use, just
  built from two explicit sample axes instead of a single stretched UV.
- **Shell march** (`main()`, right after the airglow-red block): emissive-only, additive, no
  Beer-Powder transmittance, altitude band 95-300km (green base → red/magenta fringe, color
  mixed by height fraction). Entry/exit classification copies the airglow-red march's
  `obsEffH`-keyed below/inside/above logic verbatim (same reason: the observer can fly into or
  above the shell via the uncapped elevation control, and a fixed "always below" forward root
  goes negative in that case) — clipped against `tSurface` so it doesn't render through solid
  Earth on the far side.
- **Day-gate — first pass used a single observer-based smoothstep on `pc.sunDirENU.w`, replaced
  same-day after in-app testing (see follow-up entry below) with per-sample geographic day/night,
  matching airglowRed's `rDayness`/`rNight` pattern.**
- **New UBO fields:** `CloudParams` grew 288→304 bytes — `stormStrength` (drives oval expansion,
  width, and fold frequency together — one slider, not three) and `auroraGain` (master
  brightness), plus two reserved pad floats for next time. Mirrored in `cloud_march.comp` (marked
  unused/layout-parity, per the standing rule that this UBO is hand-duplicated across both files
  and must grow in lockstep or silently corrupt cloud rendering) and in `GpuCloudParams`
  (`SatelliteSim.h`), with matching settings-window sliders ("Storm strength", "Aurora gain") and
  `settings.json` persistence (`storm_strength`/`aurora_gain`) following the exact pattern the
  airglow gains established in C15.
- **Not yet seen in-app** — build is clean but the user hasn't flown to a high geomagnetic
  latitude at night to look at it yet. First-pass constants (oval colatitude/width, fold
  frequencies, altitude band, color) are physically-motivated guesses, not eye-tuned — expect a
  tuning pass, same as every other C-step's first pass.
- Generality check against the explicit "one curtain primitive, not per-type code paths"
  requirement: future aurora types are parameter presets of the same two functions —
  `auroraOvalMask`'s width/position (wide+low-freq ripple for diffuse patches, narrow for sharp
  arcs) and `auroraCurtainNoise`'s tangent:radial frequency ratio (near 1:1 for a substorm
  spiral's swirl) — no new code paths needed for any of the deferred variants.

**Session 28 follow-up (same day) — two bugs found on first in-app look:**
- **Blown out to solid white even at `auroraGain=0.01`.** `kAuroraScale` was 0.02 — roughly 10,000×
  too large relative to `kAirglowScale` (5e-7), which multiplies an accumulation of the same order
  (segLen in meters × ~24 samples). Cut to `0.000001` (1e-6), same order of magnitude as
  `kAirglowScale` — the intended brightness difference between a faint nightglow and a prominent
  aurora belongs on the gain slider default, not the base scale. Compounded downstream by
  `EXPOSURE_NIGHT` (10×) and the Reinhard-style tonemap, which saturates every color channel to
  white together once any one channel's post-exposure value gets large — this is *why* the failure
  mode was "entirely white" rather than "overbright green."
- **Aurora vanished entirely once the observer's own local sun angle read daylight — wrong for an
  orbital view near the terminator**, where a large genuinely dark portion of the sky/limb can
  still be visible even though the observer isn't in it. The single `pc.sunDirENU.w`-based gate
  (applied once, outside the march) has been replaced with a per-SAMPLE geographic day/night test
  inside `auroraSampleAt` — literally the same `dot(pDirECEF, sunDirECEF)` twilight-window formula
  airglowRed's `rDayness`/`rNight` already uses — so each march sample fades in independently based
  on whether *that point* is geographically dark, not whether the observer is. `sunDirECEF` is now
  passed into `auroraSampleAt` (new parameter) instead of the removed observer-side smoothstep.
- Both fixes are code/math corrections, not tuning guesses — but shape/oval position/fold
  frequency still haven't been evaluated in-app since brightness was blocking any useful look at
  them. Next in-app pass should look at shape now that it's not blown out.

**Session 28 follow-up #2 (same day) — curtain fold axis was rotated 90° wrong.** With brightness
fixed, the user could finally see shape: folds were long streaks pointing radially toward/away
from the geomagnetic pole (described as looking like a "cornea"), not standing up vertically off
the surface. Root cause: `auroraCurtainNoise`'s anisotropic axes were built on a wrong analogy —
cirrus streaks stretch along a horizontal wind axis because cirrus is a flat, nearly-2D phenomenon,
so "radial = long axis" made sense there. Aurora curtains are fundamentally a *vertical* structure;
the long axis needs to be **altitude**, not colatitude (colatitude is a horizontal, toward/away-
from-pole direction on the sky, unrelated to "up off the surface"). Fixed by swapping which
coordinate carries the low frequency: `altM * kAuroraAltFreq` (now 0.00001, was tuned down slightly
for longer streaks) moved to the axis that used to hold colatitude; colatitude now only gets a
minor cross-band frequency (`kAuroraRadialFreq`, unchanged value) since the oval mask already
confines it to a narrow range — it should never be the dominant elongation axis. Tangent
(azimuthal) stays high-frequency, unchanged, since "many separate folds around the ring" was
already correct. **Lesson:** don't port an anisotropy trick between features without re-deriving
which physical axis is actually "long" for the new phenomenon — cirrus's 2D horizontal-wind
intuition doesn't transfer to a vertical curtain.

**Session 28 follow-up #3 (same day) — follow-up #2 didn't actually fix it; found the real cause.**
User re-tested: folds still radiated toward the pole, described as "very stretched in the polar
direction." The axis swap in follow-up #2 was necessary but not sufficient — it moved altitude
onto the right noise-space slot, but didn't account for **colat and altitude living in completely
different physical units**. `kAuroraRadialFreq` (6.0, unchanged since first pass) looked like a
small, reasonably-fine "minor axis" frequency next to `kAuroraAltFreq` (0.00001) — but colatitude
is in RADIANS, multiplied implicitly by Earth's radius (~6.57e6 m) to get actual physical arc
length: `1/6 rad × 6.57e6 m ≈ 1.1 million meters` — a noise cell over 1000 km across, roughly
**10x physically larger** than altitude's own ~100 km cell at the time. Numerically "low" doesn't
mean physically "long" when one axis is an angle multiplied by a planet-sized radius and the other
is already in meters — that mismatch is exactly why swapping slots didn't fix the visual: colat
remained the physically longest axis by a wide margin regardless of which noise-space component it
occupied. Fixed by recomputing all three frequencies from a target physical cell size (~50-170km)
instead of picking numbers by feel: `kAuroraRadialFreq` 6→70 (cell ≈94km, physically short now),
`kAuroraAltFreq` 0.00001→0.000006 (cell ≈167km, now unambiguously the longest), `kAuroraTangentFreq`
left at 40 (already ≈55km, was fine by coincidence). See the frequency constants' block comment in
`sat_sky.frag` for the physical-cell-size formulas. **Lesson (supersedes follow-up #2's):** when
mixing angular and linear coordinates in one noise domain, convert to a common physical unit before
judging whether a frequency is "high" or "low" — raw numeric comparison across unlike units is
worthless and will silently reintroduce this exact bug if any of these three constants get
retuned independently later without checking the others.

**Session 28 follow-up #4 (same day) — user approved the shape/brightness ("looks amazing") and
asked for three integration items before further noise tuning:**
1. **Cloud occlusion.** The aurora march ran to completion before the "Half-resolution cloud
   composite" section later in `main()`, so structurally it *was* subject to the standard
   `color = color*cloudB.rgb + cloudA.rgb` attenuate-then-add composite — but low clouds cap at
   `alphaMax=0.80` (never fully opaque by design), so up to 20% of a very bright pre-tonemap aurora
   value still leaked through even a "fully covered" deck, with no competing brightness from the
   cloud itself (nothing lit clouds from aurora yet — see item 2) to visually read as "in front."
   Added an early, redundant sample of `cloudTargetA.a` (`tCloudOcclude`, only valid once cloud
   crosses ~90% opacity) right at the top of the aurora block, clipping `atExit` by it exactly like
   `tSurface` already is. Thin/broken cloud still leaks glow through via the existing multiply —
   correct — only genuinely dense cloud now hard-cuts the march.
2. **Ambient lighting for clouds/terrain/ocean.** Rather than duplicating the full oval-mask +
   anisotropic-fold-noise curtain machinery (sat_sky.frag-only) into `cloud_march.comp` just for a
   soft ambient wash, added a cheap CPU-side proxy: `SatelliteSim.cpp` now computes
   `auroraGroundGlowRaw` once per frame from the OBSERVER's own position (mirrors
   `kGeomagPoleECEF`/oval-mask math, ripple/fold omitted — unnecessary at ambient fidelity; reuses
   `updateStars()`'s existing `nightFactor` shape for "how dark right now"). Uploaded via
   `CloudParams.auroraGroundGlow` (renamed from the reserved `pad4`) alongside a new user slider
   `auroraGroundGain` (renamed from `pad5`) — no UBO growth needed, both pads were still free.
   `cloud_march.comp`'s `cloudMarchCS` adds an `auroraUp` term to `inScatter`, height-weighted
   OPPOSITE to `cityUp` (aurora is 95-300km up, far above the 2-11km cloud shell, so it lights cloud
   TOPS more than bases — cityUp washes bases from below). `sat_sky.frag`'s terrain block adds an
   up-facing-weighted `auroraContribTerrain` to `surfColor`; the ocean block adds a flat,
   `atten`-falloff-weighted term near the moon-glint code. Cirrus (`cirrusMarchCS`) was left out of
   scope — it has no ambient-lighting infrastructure at all (sun-only inScatter) and is the much
   less visually dominant layer (`alphaMax=0.15` vs. low cloud's `0.80`); can be added later if it
   turns out to matter.
3. **Evolution speed.** `kAuroraOvalDriftRate` 0.03→0.003 and `kAuroraShimmerRate` 0.25→0.025 (both
   10x slower per explicit user ask) — the oval ripple and vertical shimmer were animating too fast
   for something the size of a continent-spanning curtain.

**Session 28 follow-up #5 (same day) — item 2's ambient lighting was rebuilt from scratch; the
CPU-observer-based proxy was the wrong model entirely.** User tested from LEO: passing over the
oval colored the ENTIRE VISIBLE EARTH green uniformly, and it snapped back to pitch black the
instant the observer's orbit left the oval — regardless of what was actually under the curtain at
any given point on the ground. Root cause: follow-up #4's `auroraGroundGlowRaw` was computed once
from the OBSERVER's own position and applied as a single flat multiplier to every terrain/ocean/
cloud sample in view. That's backwards — moonlight (the explicit model to match) is local: a
`geoMoonDot`/horizon-gate check happens AT EACH SURFACE POINT using that point's own geometry, not
the observer's. Rebuilt to match:
- **Removed the CPU proxy entirely** — `SatelliteSim.cpp`'s `auroraGroundGlowRaw` computation
  deleted, `CloudParams.auroraGroundGlow` reverted back to reserved `pad4` in all three mirrored
  structs (`sat_sky.frag`, `cloud_march.comp`, `GpuCloudParams`). `auroraGroundGain` (the slider)
  kept — its meaning shifted from "gain on a CPU scalar" to "gain on a per-point GPU evaluation."
- **`sat_sky.frag`: new `auroraGlowAt(posDirECEF, sunDirECEF, t, storm)`.** Runs the SAME
  `auroraFrame`/`auroraOvalMask`/`auroraCurtainNoise` the sky curtain itself uses, but keyed on
  the ARGUMENT direction (a ground point's own `normalize(hitPt)`) instead of the observer's
  position — same per-sample day/night gate `auroraSampleAt` already uses. Terrain and ocean
  ambient terms now call this with their own hit point, so only ground actually under an active
  curtain lights up, independent of where the observer is.
- **Ocean also gained a genuine REFLECTION term**, per explicit user request ("visible in the
  reflection shaders... local and interact with the existing lighting shaders"): inside the
  existing sky-reflection block (the 6-sample atmosphere march along `reflDir`), added a second
  small march (6 samples) using `auroraSampleAt` along that SAME reflected ray — the aurora is now
  a literal mirror-like glint on wave faces that happen to reflect toward the curtain, not just a
  flat wash, using `cloud.auroraGain` (the sky curtain's own brightness) since it's genuinely
  reflecting that same light, not a separate ambient source.
- **`cloud_march.comp`: new local `auroraOvalMaskLocal(posDirECEF, storm)`** — a deliberately
  stripped-down oval mask (no ripple warp, no fold noise) since it evaluates once per IN-CLOUD
  march sample, the hottest loop in the renderer; fold-noise fidelity wasn't worth the cost here.
  Uses each sample's own `dirECEF` (already computed in the loop) instead of a CPU scalar, gated by
  `sampleDayness` like `cityUp` already is.
- **General lesson (same shape as follow-up #3's, different layer of the stack):** "evaluate once,
  apply everywhere" only works when the phenomenon really is uniform across everything in view —
  true for the observer's OWN sky brightness (a valid simplification used elsewhere in this
  codebase), false for lighting that varies by GEOGRAPHIC location. Ground/cloud/ocean lighting
  needs to be computed at the location being lit, not the location doing the looking. When in
  doubt, check what the equivalent moonlight/sunlight code does — it was already doing this
  correctly and should have been the template from the start instead of inventing a new
  "CPU-computed scalar" pattern for aurora specifically.

Not yet seen in-app — build is clean but neither follow-up #4's cloud-occlusion/evolution-speed
changes nor follow-up #5's rebuilt lighting model have had an in-app look yet.

**Session 28 follow-up #6 (same day) — first in-app look at follow-up #4/#5, four more issues:**
1. **Aurora rendered behind clouds even from LEO looking down, where it should be in front.**
   Root cause: the compositing model assumed a fixed depth order (aurora always farther than
   clouds) baked into the code structure itself — the aurora march's contribution was unconditionally
   folded into `color` BEFORE the later `color = color*cloudB.rgb + cloudA.rgb` cloud composite, so
   clouds always got treated as being in front. True from the ground (clouds 2-11km sit between the
   camera and the 95-300km aurora shell) but backwards from LEO+ looking down, where the aurora
   shell is entered FIRST (closer to the camera) and clouds are much farther along the ray, near the
   surface. Fixed by comparing the aurora march's own entry distance (`atEnter`) against the cloud
   layer's entry distance (`tEnterCombined`, sampled early via a texture fetch purely for this
   comparison) and branching: if aurora is farther than clouds, merge its contribution into `color`
   as before (pre-composite, so the smooth multiply attenuates it correctly); if aurora is nearer,
   defer it into `auroraContribDeferred` and add that AFTER the cloud composite line instead, so
   clouds don't wrongly occlude an aurora that's actually in front of them.
2. **Blocky aliasing at the cloud/aurora edge — this was follow-up #4's own `tCloudOcclude` hard
   clip, and it was a mistake.** `tCloudOcclude` is a HALF-RESOLUTION, bilinearly-sampled field
   that's discontinuous by construction (-1 where cloud isn't opaque enough, a real distance where
   it is) — interpolating across that boundary produces bogus intermediate values, and using the
   result as a hard `min()` cutoff for a full-resolution march bakes the half-res texel grid's
   quantization directly into the aurora's visible edge. Removed entirely; the existing smooth
   multiplicative composite (now correctly ordered per fix 1) is the only occlusion mechanism —
   thin cloud lets some glow through, which is physically fine, and there's no longer a hard edge
   to alias.
3. **Cloud/terrain aurora brightness couldn't share one gain slider.** At a ground gain low enough
   for clouds to look plausible (~0.004), terrain contribution was near-zero — because the cloud
   formula has no albedo term at all (assumes ~full reflectivity) while terrain/ocean multiply by
   the surface's own (often much darker) `dayColor`, so the same raw light value produces wildly
   different visual magnitude through the two formulas. Split `auroraGroundGain` (terrain/ocean
   only now) from a new `auroraCloudGain` (clouds only), using the CloudParams' last free pad slot
   — no UBO growth. Defaults: `auroraCloudGain=0.02`, `auroraGroundGain` unchanged at `1.0`; both
   will still need in-app tuning, this only unblocks independent control.
4. **Erosion/patchiness, per explicit user request** ("cut this up and erode with another noise
   pattern to emulate lines and curves of twisty turning aurora"). The existing `auroraCurtainNoise`
   fold texture only varies brightness WITHIN an already-lit patch — it doesn't create large gaps
   where NO aurora exists at all, which is why the oval read as evenly lit all the way around.
   Added `auroraCoverage(az, t, storm)`: a much-lower-frequency (`kAuroraCoverageFreq=4` vs. fold's
   `kAuroraTangentFreq=40`) `warpPerlin3` threshold gate multiplied into `auroraOvalMask` itself, so
   whole multi-degree stretches of the ring can have zero aurora at all — real auroral activity does
   look like broken arcs, not a solid ring, especially at lower storm strength (the threshold
   `mix(0.2, -0.6, storm)` fills in gaps as storm strength rises, matching how strong substorms
   really do brighten/fill the whole oval). Mirrored into `cloud_march.comp`'s
   `auroraOvalMaskLocal` too (now takes `az` — computed via a small inline tangent-frame calc,
   mirroring `auroraFrame`) for visual consistency between the sky curtain and cloud-underside
   lighting; NOT mirrored into `auroraSampleAt`'s pole-relative math since that shares
   `auroraOvalMask` directly and picks the change up for free.
   **Implementation note:** `auroraOvalMaskLocal` had to move to right after `warpPerlin3`'s own
   definition in `cloud_march.comp` — GLSL has no forward declarations, and the function now calls
   `warpPerlin3` for the coverage noise.

Not yet seen in-app.

**Session 28 follow-up #7 (same day) — second in-app look at #6, three more findings:**
1. **Aurora STILL rendered behind clouds wrongly, now at ground level too (not just LEO), and the
   correct window narrowed to roughly 260-330km only.** The per-ray distance comparison from
   follow-up #6 (`atEnter` vs. an early-sampled `tEnterCombined`) was replaced outright with a much
   simpler, provably-correct rule: **the ordering is purely a function of the OBSERVER's own
   altitude**, not a per-ray comparison at all. Clouds (2-11km) and the aurora (95-300km) occupy
   fixed, non-overlapping altitude bands around Earth — below 95km, ANY ray that reaches both must
   cross the (lower) cloud band before it can climb to the (higher) aurora band; at or above 95km,
   any downward ray crosses the aurora band before it can reach the lower cloud band. There is no
   ray-by-ray ambiguity once the observer's altitude is fixed, so `auroraBehindClouds = (obsEffH <
   kAuroraShellInnerM)` replaces the whole distance-comparison block — cheaper (no extra texture
   sample) and has no per-ray edge cases left to get wrong. The prior comparison's exact failure
   mode was never fully root-caused (the ground case, in particular, looked correct when hand-traced
   through the same math) — this fix sidesteps needing to find it rather than patching a fragile
   mechanism further. This also explains the odd "260km perfect, above 330km fades wrong again"
   window: 95-300km always forced `atEnter=0` (observer inside the shell), which coincidentally
   produced the right answer only inside the aurora's own altitude band — the fix removes that
   coincidence entirely by not depending on shell-relative branching for the comparison at all.
2. **Erosion "cranking up frequency" produced lines tracing straight at the pole, not arcs parallel
   to latitude.** Root cause fully understood this time (not a guess): `auroraCoverage` sampled
   noise as `f(az)` only — CONSTANT along colatitude. A field that's constant along colat and varies
   with az has its threshold crossings sitting on constant-azimuth CONTOURS — and constant-azimuth
   lines are meridians, which by definition point straight at the pole. Any real coverage frequency
   was always going to look like radial spokes; higher frequency just added more of them. Swapped
   which coordinate dominates: `auroraCoverage` now varies primarily with COLATITUDE
   (`kAuroraCoverageFreq=0.35`, per-degree, ~2 cells across a typical 6-15° band width) with only a
   small azimuthal term (`kAuroraCoverageAzFreq=1.5`, a gentle large-scale wave so the boundary
   isn't a perfect circle, not enough to reintroduce spokes). Threshold crossings now fall on
   near-constant-colatitude contours — parallel to latitude circles, matching "large tracks...
   parallel with latitude lines" exactly as asked. Mirrored in `cloud_march.comp`.
3. **Milky Way also renders in front of clouds — NOT shared code with clouds, a separate pre-
   existing design choice from session 27, unrelated to the aurora work.** The Milky Way skybox
   term is added post-TONEMAP (see its own comment: "Added post-tonemap like the ambient terms
   around it... rather than folded into the HDR atmosphere accumulation above") — it was never
   composited against cloud opacity at all, by design, since it's meant to be a cheap, always-on-top
   approximation. This wasn't visible as a problem before because clouds+visible-Milky-Way at the
   same time wasn't something anyone was specifically testing until now. Fixing it properly means
   moving the Milky Way's color computation to before the tonemap step and folding it into `color`
   pre-cloud-composite (the same pattern the aurora fix above uses) — a distinct, separable change
   from the aurora work, not yet done pending user confirmation they want it addressed now.

Not yet seen in-app.

**Session 28 follow-up #8 (same day) — erosion praised as "a lot better" after follow-up #7's
axis swap, but one more bug and a tuning-workflow request:**
1. **All coverage patches tracked from low latitude toward the poles ("waves of energy" instead of
   undulation).** Same class of bug as follow-up #7's diagnosis, one layer deeper: `auroraCoverage`
   built its `warpPerlin3` sample point as `vec3(azWarp, colatDeg*freq + t*driftRate)` — time was
   added directly onto the SAME coordinate as colatitude. Advancing time was therefore
   mathematically identical to advancing colatitude: the entire pattern visibly translated
   toward/away from the pole every frame, which reads exactly as "waves marching to the poles."
   Fixed by moving the time term onto the azimuthal embedding instead: `azWarp = vec2(cos(az),
   sin(az))*azFreq + t*driftRate`. This is safe there in a way it wasn't on the colat axis — x/y
   here are an arbitrary 2D embedding of azimuth (chosen only to avoid a seam at az=±π), not a
   coordinate with real "distance from pole" meaning, so translating them doesn't correspond to any
   recognizable directional slide; the noise at a fixed (az,colat) point instead genuinely
   evolves/shimmers over time. **General pattern to watch for going forward: any time term MUST
   land on an axis with no fixed physical meaning (or genuinely wants to slide, like the ripple's
   own azimuthal drift) — never on an axis that represents a real direction (colatitude,
   altitude) unless a directional slide is actually the intended look.**
2. **User asked how to tune coverage frequency/speed** — previously hardcoded constants requiring a
   shader recompile per attempt, too slow for iteration. Promoted `kAuroraCoverageFreq` (patch size,
   per-degree colatitude frequency), `kAuroraCoverageAzFreq` (azimuthal wobble frequency), and
   `kAuroraCoverageDriftRate` (evolution speed) from constants to `CloudParams` UBO fields — grew
   304→320 bytes (4 new floats, one reserved pad) with matching settings-window
   sliders ("Coverage freq"/"Coverage az freq"/"Coverage drift") and `settings.json` persistence.
   Edge softness (`kAuroraCoverageSoftness`) stayed a fixed constant — not worth its own slider.
   Defaults carried forward the values the user had already hand-tuned directly in
   `cloud_march.comp` before this change (`freq=0.65`, `driftRate=0.00008`) rather than resetting
   to the original guesses, so their prior tuning isn't lost.

Not yet seen in-app.

**Session 28 follow-up #9 (same day) — user's current in-app tuning promoted to defaults, plus
four more fixes and a UI reorganization:**

0. **Defaults promotion.** User: "my current debug settings should be our defaults going forward."
   Read `build/Debug/settings.json` and copied every cloud/ocean/aurora/photometry value into the
   corresponding member-variable initializer in `SatelliteSim.h` (not observer lat/lon, camera, or
   other session-specific state — just the rendering/tuning parameters). Notable swings from the
   old placeholder defaults: `cloudMarchSteps` 138→4, `auroraGain` 1.0→0.0785, `lightPollutionGain`
   1.0→38.6, `daySuppression` 1516→574.6 — all intentional, all copied verbatim from the user's
   tuned settings.json, not independently re-derived.
1. **Aurora AND Milky Way still drew over clouds at ground level — user correctly guessed this was
   related to clouds' own Mie-scattering ambient term, not a depth-order bug** (the altitude rule
   from follow-up #7 is provably correct and wasn't the issue here). Found a real, concrete bug:
   `cloud_march.comp`'s `skyAmbient` (the cloud's zenith Rayleigh+Mie ambient light) was the ONLY
   ambient contributor NOT gated by day/night — `moonContrib` (via `nightFac`), `cityUp`, and
   `auroraUp` all correctly fade at night; `skyAmbient` didn't. Physically it represents
   zenith-scattered SUNLIGHT, which genuinely is zero at true night — leaving it ungated let unlit
   clouds read as a lit, sky-colored haze instead of a dark silhouette, making them visually
   indistinguishable from "more sky" rather than a solid foreground object occluding the aurora/
   Milky Way behind them. Fixed by multiplying by `(1.0 - sampleDayness)`, matching cityUp/auroraUp's
   existing gate. Separately, Milky Way's own "renders through clouds" issue has a different cause:
   it's added post-TONEMAP by design (session 27, "comparably faint... rather than folded into the
   HDR atmosphere accumulation") and was NEVER composited against cloud opacity — not shared code
   with clouds, a distinct pre-existing simplification. Restructuring it to merge pre-tonemap (the
   "correct" fix) was judged too large/risky to bundle in; instead multiplied its existing
   post-tonemap contribution by `cloudBlock` (the same already-computed opacity scalar that already
   dims the sun disc through clouds) — a contained, low-risk suppression that stops it showing
   through opaque cloud without touching the tonemap pipeline.
2. **No knob for curtain fold noise evolution, and columns "flicker top to bottom pretty
   consistently" instead of undulating** — the SAME axis-conflation bug as follow-up #8's coverage
   fix, one layer down: `auroraCurtainNoise` added `t * kAuroraShimmerRate` directly onto the
   ALTITUDE coordinate (a real physical axis — "up"), so advancing time was mathematically
   identical to sliding the fold pattern vertically. Moved the time term onto the TANGENT/azimuthal
   axis instead (already high-frequency, "many folds around the ring" by design) — folds now
   ripple/drift sideways over time instead of scrolling monotonically top-to-bottom, much closer to
   how real curtains dance. Promoted `kAuroraShimmerRate` from a constant to `cloud.auroraShimmerRate`
   (settings slider "Fold shimmer rate") using the CloudParams' last free pad slot — no UBO growth.
   **Third occurrence of this exact bug class in this feature (colat in follow-up #8, altitude
   here) — worth double-checking any remaining noise calls in this file for the same mistake before
   adding more.**
3. **Banding/artifacts at grazing (near-horizontal) and steep look-down angles** — same root cause
   already fixed once for clouds (session 22, `kCloudMaxStepM`): the aurora march used a FIXED
   `N_AURORA=24` regardless of path length through the shell, but that path length varies enormously
   with viewing angle — ~205km looking straight up, potentially thousands of km near-horizontal.
   A fixed step count spreads across whichever one it is, badly undersampling the fold noise's own
   ~55-170km physical cell size at grazing angles. Made step count adaptive to path length
   (`clamp(int(pathLen / 15000.0), 24, 160)`), capped both ends — matches the established pattern
   from cloud march perf work rather than inventing a new one.
4. **UI reorganization:** the "Clouds" settings tab had grown to 33 sliders spanning clouds, ocean,
   terrain/atmosphere quality, airglow, and aurora — split into 4 tabs (Clouds/Ocean/Terrain/Aurora,
   settings tab count 8→11). Extracted the shared per-row Clay rendering loop into
   `buildCloudSliderRows()` (a private member taking a `CloudSlider*` array + count) so the split
   didn't require 4 copies of ~80 lines of slider-widget code; each tab's slider list keeps its
   ORIGINAL global `idx` (used to key the shared `draggingCloud`/`hovCloudMinus`/`hovCloudPlus`
   arrays and a function-local static text-buffer), so no renumbering was needed despite splitting.
   Shared-ownership sliders were assigned a single home: view/light sample counts (main atmosphere
   loop, runs on every pixel) → Terrain; `moonGain` (terrain direct term AND cloud moonContrib) →
   Terrain. **Also fixed a latent out-of-bounds bug found while doing this:** `hovCloudMinus`/
   `hovCloudPlus` were still sized `[25]` from before the C16 aurora sliders were added, while
   slider indices already went up to 32 — every aurora slider's +/- button was reading/writing past
   the end of those arrays. Bumped both to `[33]`.

Not yet seen in-app.

**Session 28 follow-up #10 (same day) — "so close": three more fixes, all from a single in-app
pass, all previously-latent limitations rather than newly-introduced bugs:**
1. **Clouds near the horizon let the aurora/Milky Way/stars straight through, even though they
   correctly occlude at the zenith.** Not a per-step opacity bug — `cloudMarchCS`'s
   `cloud.maxRenderDistM` (a hard cap on how far along the view ray the march continues) was
   defaulting to 165km, but the low-cloud shell's own geometric horizon distance at 11km altitude
   is `sqrt(2·R_EARTH·11000) ≈ 374km` — the march simply stopped accumulating optical depth barely
   a third of the way to the shell's true horizon, so everything beyond that distance fell back to
   "no cloud here" regardless of what the geometry actually looked like further out. Raised the
   default to 400km and the settings slider ceiling 400km→800km; also raised the per-march
   `hardCap` iteration ceiling 2048→4096 so it doesn't become the new binding limit once
   `maxRenderDistM` is pushed that far. **Cirrus had the identical bug independently** — a
   hardcoded, UBO-unrelated `200000.0` cap in `cirrusMarchCS`, while cirrus's own default 15km
   altitude gives a horizon distance of `≈437km`. Raised to 450km — but doing that alone would have
   traded "cirrus disappears at the horizon" for "cirrus bands at the horizon," since `N_CIRRUS` was
   a FIXED 14 steps spread across whatever the path length turned out to be (the exact class of bug
   already fixed for the aurora march in follow-up #9). Made `N_CIRRUS` adaptive to path length the
   same way (`clamp(pathLen/3000, 14, 128)`) so extending the range doesn't reintroduce undersampling.
2. **Green/sodium airglow had a hard edge where the aurora oval clipped against it** — both
   occupy a similar altitude band, and the aurora's `auroraOvalMask` fell from full brightness to
   exactly zero across its whole nominal width (`widthDeg`), while airglow has no matching cutoff
   at all (it's a uniform whole-sky Gaussian-in-altitude term) — so stepping across the oval's edge
   showed airglow's steady baseline plus a value that hit zero abruptly, reading as a seam. Fixed
   by keeping the same-size bright "core" (now `widthDeg*0.5`) but extending the fade-out zone much
   further before it reaches zero (`widthDeg*2.0` instead of `widthDeg*1.0`) — same overall footprint,
   much gentler edge gradient, blends into airglow instead of clipping against it.
3. **Aurora visibility from the ground wasn't affected by city light pollution or moonlight at
   all**, unlike every other faint-sky-glow phenomenon in this renderer (stars, Milky Way, satellite
   flares all dim under both). Added the identical suppression: the directional 16-sector
   `lightDome[]` lookup (same buffer `sat_flare.comp`/`updateStars()` consume) using the aurora's
   own view-ray bearing, and the same elevation²×illumination moon-brightness shape used elsewhere
   — duplicated rather than shared (this runs earlier in `main()` than the Milky Way block that has
   its own copy), matching this file's established one-copy-per-consumer precedent for this exact
   formula. `kAuroraPollutionMaxDim=0.9`/`kAuroraMoonMaxDim=0.85` cap how much either source can dim
   it, same "hard ceiling regardless of gain" convention `kSatPollutionMaxDim`/`kStarMoonMaxDim`
   already use.

Not yet seen in-app.

**Session 28 follow-up #11 (same day) — user asked for a full audit after the extended render
distance still didn't stop aurora/Milky Way showing through cloud, even off-horizon.** Traced the
entire chain end to end: aurora's depth-order/merge logic (follow-up #10's altitude rule),
the cloud composite (`color = color*cloudB.rgb + cloudA.rgb`), and the Milky Way's own
`* cloudBlock` — all structurally correct, confirming the compositing ORDER wasn't the remaining
problem. The real issue: **`cloudB.rgb`/`cloudBlock` is a LINEAR transmittance value, and aurora
(HDR, further amplified by `EXPOSURE_NIGHT`'s 10x at night) and the Milky Way are bright enough
that even a cloud reading as "mostly opaque" (transmittance ~0.25) still shows clearly through at
a quarter strength** — a genuine, previously-unaddressed gap, not a depth-order or render-distance
bug at all (extending the render distance in follow-up #10 was still correct and necessary, just
not sufficient on its own). Fixed by applying an explicit CUBED suppression (`pow(transmittance,
3.0)`) specifically to aurora and Milky Way — 0.25³≈0.016 (thick cloud now dramatically more opaque
to them) vs. 0.9³≈0.73 (thin/absent cloud barely affected) — steepening the falloff without
introducing a new discontinuity, since it's applied to the same continuous, smoothly-interpolated
value the standard composite already uses (not the discontinuous `tCloudOcclude` distance field
that caused aliasing in an earlier, since-reverted attempt at follow-up #7). Restructured aurora's
own resolution to make this cleaner: it now ALWAYS resolves through `auroraContribDeferred` (added
once, after the cloud composite), rather than sometimes merging into `color` pre-composite to pick
up the linear multiply — avoids the two suppression mechanisms (linear composite + cubed factor)
compounding unpredictably. When aurora is in front of clouds (LEO+ looking down), no cloud
suppression is applied at all, unchanged from before.

Not yet seen in-app.

**Session 28 follow-up #12 (same day) — opacity fix confirmed working; two follow-on reports, one
confirmed bug, one checked-and-clean.** After follow-up #11, clouds now properly occlude the aurora
curtain and Milky Way. Remaining reports: (1) near the horizon, a residual aurora GLOW (not the
volumetric curtain — that's correctly gone) still showed; user correctly diagnosed this as the
ground-glow/ambient code, separately noting the noise pattern "stays fixed with the observer as
they move, instead of following the terrain." (2) "A similar effect... within the cloud shadows...
glued to the user perspective."

**(1) Confirmed and fixed — a real coordinate-frame bug.** Terrain's `auroraContribTerrain` and
ocean's ambient aurora tint both called `auroraGlowAt()` — which computes colatitude/azimuth
relative to the FIXED geomagnetic-pole ECEF constant, so it needs a true ECEF direction — but
passed `normalize(hitPt)` / `surfUp` directly. Both of those are in the observer-local ENU-ish
frame (the same convention `obsPos`/`dir`/march points all use in this file), NOT ECEF. Every OTHER
geographic lookup in this file (terrain lat/lon UV, `sunDirECEF`, the airglow/aurora sky march
itself) explicitly converts through the `enuX`/`enuY`/`enuZ` basis first; this one skipped that
step. The practical effect: the "geographic position" fed into the oval-mask/fold-noise math was
actually the point's position *relative to the observer*, which shifts every time the observer
moves — exactly "pattern follows the observer, not the terrain." Fixed by converting both to true
ECEF (`hitDirECEF = normalize(hitPt.x*enuX + hitPt.y*enuY + hitPt.z*enuZ)`, same for `surfUpECEF`)
before the `auroraGlowAt` calls. Note: the Lambertian weighting term right next to the terrain bug
(`dot(shadingN, normalize(hitPt))`) was NOT part of this bug — `shadingN` is itself in the same
local frame, so that dot product was already internally consistent; only the `auroraGlowAt` input
needed the fix.

**(2) Checked, not reproduced in code — cloud_march.comp's aurora upwelling term is already
correctly ECEF-anchored.** Traced its `dirECEF` back to `pECEF = p.x*enuX + p.y*enuY + p.z*enuZ`
where `p` is the current march sample position — this reconstructs the sample's TRUE geographic
position every frame from the observer's actual current position/orientation, the same pattern
confirmed correct for terrain above. No coordinate-frame bug found here on inspection. Plausible
explanation: the terrain/ocean bug in (1) was prominent enough (glow visibly sliding with observer
motion) that it read as affecting the whole scene, clouds included, even though the cloud-side code
itself checks out. Flagged as unresolved — if the "glued to perspective" cloud effect persists after
this build, it's a genuinely separate bug (self-shadow cone / cloud-shape drift synchronization is
one candidate not yet investigated) rather than the same root cause as (1).

Not yet seen in-app.

**Session 28 follow-up #13 (same day) — terrain glow fix confirmed working; user reported one more,
correctly self-diagnosing the mechanism.** "The auroral glow (and sky Mie scattering for the most
part) dramatically overlays clouds over the horizon... I believe this bug is within our ambient
skyglow + Mie scattering on clouds approach" — pointing directly at `skyAmbientBase` in
`cloud_march.comp`. Confirmed: `skyAmbientBase` (the cloud's zenith Rayleigh+Mie ambient term,
built from a small 6-step sub-march) was sampled from `p0 = obsPos + tEnter*dir` — the cloud
march's ENTRY point — rather than the observer's own position. For a steep/near-vertical ray this
is fine (`tEnter` small, close to the observer), but for a GRAZING/near-horizontal ray `tEnter` can
be hundreds of km out, placing `p0` near the observer's own visual horizon. Earth's curvature means
conditions there (day/night state, twilight angle) can genuinely differ from the observer's own —
and since `skyAmbientBase` is computed ONCE per pixel and then applied UNIFORMLY to every in-cloud
step across the whole march (now up to ~400km after follow-up #10's render-distance extension), a
single potentially-unrepresentative sample from a distant point was getting smeared across the
entire visible horizon band. This is exactly why it surfaced now: the render-distance fix that
correctly stopped clouds from vanishing at the horizon simultaneously stretched how far this
single-sample approximation gets applied, from ~165km (mild) to ~400km (dramatic). Fixed by
anchoring `p0` to `obsPos` instead — a stable read of the observer's own actual local conditions
(the thing that's genuinely dark during an aurora-visible night) rather than whatever a distant
point along the ray happens to see. `sunColorCloud` has the identical `obsPos + tEnter*dir` pattern
right above this block but was deliberately left unchanged: it computes DIRECT sun color reaching a
specific cloud position (sunset/sunrise spectral shift), which is legitimately position-dependent —
unlike ambient sky brightness, "moving" it to `obsPos` would make sunset cloud coloring LESS
accurate, not more, and it isn't implicated in the reported symptom (aurora/Mie ambient glow at
night, not daytime sun coloring).

Not yet seen in-app.

**Session 28 follow-up #14 (same day) — the skyAmbientBase fix didn't resolve the horizon
transparency; two side findings and two confirmed march-budget bugs.**

**Side finding #1 (resolved a false lead):** user reported aurora-looking noise "on the terrain,"
distinct from the horizon-glow issue, unaffected by `auroraGroundGain=0`. Traced to the correct
cause: the main aurora sky curtain march (gated by `cloud.auroraGain`, a completely different
control from the ambient `auroraGroundGain`) clips at terrain (`atExit = min(atExit, tSurface)`)
ONLY when terrain is closer than where the curtain begins — for terrain far enough away (past the
curtain's own ~95-374km near boundary, i.e. most of the horizon), the march still runs and adds its
result on top of that terrain's already-rendered color. User confirmed: setting the SKY `Aurora
gain` (not ground gain) to 0 made the terrain-glow disappear too. Not a bug — this is the real
curtain, correctly Earth-anchored, legitimately visible in the sky in front of distant terrain: a
depth-legibility question (does it read as "floating above the distant landscape" vs. "painted on
it"), not a compositing bug, and not pursued further this round since the user's follow-up moved on
to the still-unresolved main issue.

**Side finding #2 (redirected the investigation):** with the sky curtain ruled out as the cause of
the terrain glow, the user's REMAINING, still-open report is specifically: clouds stay transparent
near the horizon even after follow-up #10's render-distance extension. User asked directly: "are we
not casting enough to determine a cloud is dense near the horizon?"

Analysis: the front-to-back volumetric accumulation formula `cloudScatter = Σ Tᵢ·(1-stepTᵢ)·inScatter`
is a telescoping sum — `Σ Tᵢ·(1-stepTᵢ) = 1 - T_final` REGARDLESS of step count, so per-step
resolution shouldn't by itself change the FINAL accumulated opacity, only banding/noise quality
(already addressed for aurora/cirrus in follow-up #9/#10). What DOES matter is whether the march
covers the FULL intended distance before running out of iteration budget. Checked both budgets
against their own render-distance caps and found two confirmed, fixable bugs:
- **Cirrus: `kCirrusStepsMax=128`, set in the SAME edit that raised the render-distance cap to
  450km, was never rechecked against it.** 450000m / kCirrusMaxStepM(3000m) = 150 steps needed to
  cover the full distance at the intended resolution — 128 is short of that, silently forcing
  grazing/horizon cirrus into a coarser-than-intended ~3516m step. Raised to 200 for margin.
- **Low cloud: `hardCap`'s flat `+32` margin left only 32 spare iterations at the 400km default**
  (1600 steps exactly needed to cover it at kCloudMaxStepM=250m, continuously in-cloud — precisely
  the dense-horizon-cloud case this exists to handle). Changed to a 15% multiplicative margin plus a
  larger flat floor (`+64`) so it scales with `maxRenderDistM` instead of staying fixed, and raised
  the absolute ceiling 4096→8192.

Both are real, confirmed budget-insufficiency bugs, not guesses — but whether they're THE
explanation for the reported horizon transparency (vs. a coverage-texture/density-data issue, which
would need different tuning rather than a code fix) is not yet confirmed in-app.

Not yet seen in-app.

**Session 28 follow-up #15 (same day) — user asked directly whether the aurora's own emissive
magnitude, not cloud opacity, was the real culprit. It was — a genuinely missing physical effect,
not a tuning number.** Checked whether the aurora's brightness respects atmospheric extinction the
same way every other faint sky object in this renderer does (satellites via `sat_flare.comp`, stars
via `updateStars()`, even the Milky Way's own block in this same file) — it didn't. The Kasten &
Young 1989 airmass model (`cloud.extinctionCoeff`, already piped in and used by all three of those)
had simply never been applied to the aurora's own emitted brightness. Real aurora viewed at low
elevation, through dramatically more atmosphere, should dim (and redden) the same way — omitting
this meant NOTHING reduced the curtain's own brightness for viewing angle, so a horizon view stayed
exactly as bright as looking straight up. At `elDeg=0` the airmass formula gives ≈38 (vs. 1 at
zenith); at the current tuned `extinctionCoeff≈0.37`, that's `extinctMag≈13.6`, i.e. a ~10⁵-10⁶×
dimming factor right at the true horizon — this is very likely the dominant reason the curtain
stayed visible through clouds even after follow-up #11's cubed cloud-suppression: the thing shining
through was simply far too bright to begin with, not under-suppressed by the clouds. Added to both
the primary sky march (using the view ray's own elevation, gated by `atmFracAurora` so it fades out
for orbital observers with no atmospheric column left) and the ocean reflection glint added in
follow-up #10-#11 (using the REFLECTED ray's elevation instead, since that's the direction the
light actually traveled through the atmosphere before bouncing off the water — ocean views skew
toward low-angle reflections by Fresnel construction, so this matters at least as much there).
Considered but did NOT add a hard cap on the aurora march's own geometric chord length through the
95-300km shell (unlike clouds' `maxRenderDistM`, the aurora march has none — a grazing ray's path
through the much taller, much thicker shell can be far longer than a zenith ray's ~205km, and
brightness scales roughly with that path length) — the extinction fix's magnitude at true-horizon
angles is dramatic enough that this is very unlikely to still be needed, and adding an artificial
cap without confirming it's actually still required would be scope creep with no in-app evidence
yet to justify it.

Not yet seen in-app.

**Session 28 follow-up #16 (same day) — the extinction fix confirmed working (clouds now properly
occlude the aurora at the horizon, airglow edge also reads better) — but the SAME fix introduced a
new, narrower regression: aurora visibly fading out as the observer travels through the
airglow/aurora altitude band (~90-100km), where it should stay strong.**

Root cause: `atmFracAurora` (the gate that fades extinction out for elevated observers, since there's
progressively less atmospheric column left to attenuate through) reused the SAME 80km scale height
`sat_flare.comp`/`updateStars()`/the Milky Way block already use — but that value was calibrated for
a completely different distinction (ground vs. LEO, ~400-600km observers), not for fading out by the
aurora's own ~95km operating altitude. `exp(-95000/80000) ≈ 0.31` — nowhere near negligible — so an
observer flying up toward or through the aurora/airglow shell saw the curtain progressively (and
wrongly) dim as they approached it, exactly backwards: at 95-100km there is essentially NO
atmospheric column left between the observer and nearby curtain material, so extinction should be
close to zero right there, not 30% strength. Fixed by using a dedicated, much shorter 20km scale
height for this specific gate (`exp(-95000/20000) ≈ 0.009`, negligible well before reaching the
shell) — the 80km value elsewhere is untouched, this is a separate constant tuned for aurora/airglow
specifically rather than reused from a use case with a different relevant altitude range.

User separately noted a relative-brightness preference — aurora should read as more prominent than
airglow where the two visually overlap — which may have been partly a symptom of the same
over-extinguished-aurora bug (airglow has no equivalent extinction term at all, so it wasn't being
dimmed the same way); worth a fresh look after this fix before treating it as a separate gain-tuning
request.

Not yet seen in-app.

**Session 28 follow-up #17 (same day) — extinction-scale-height fix confirmed working, but exposed
a DIFFERENT hard edge at the same ~95km altitude: the aurora's own inner shell boundary
(`kAuroraShellInnerM`), a real, always-there limitation that had just been masked until now.**

`auroraSampleAt`'s `vert` (vertical density) term was `smoothstep(kAuroraShellInnerM,
kAuroraShellInnerM+15000, altM)` — a ONE-SIDED ramp: exactly zero at 95km, full by 110km, with
ZERO tolerance below 95km at all. Airglow's green/sodium bands peak at 90-96km with real presence
well below 95km (down to ~83-87km at their Gaussian half-widths) — no matching cutoff. Aurora going
abruptly to nothing right at its own nominal base, with no soft entry, read as a seam against
airglow's continued presence there — including the specific case the user called out, where Earth's
curvature puts a lower-altitude portion of the visible curtain geometrically "behind" the airglow
along a given sightline, where it should still show through fading rather than vanish outright.

Two changes were needed, not one — softening the DENSITY FORMULA alone wasn't sufficient:
1. **`vert`'s inner edge widened to a SYMMETRIC 15km transition** centered on `kAuroraShellInnerM`
   (80-110km) instead of the one-sided 95-110km ramp.
2. **The march's own geometric bounds (in `main()`) had to widen to match** — `tAuroraIn` (the
   raySphere call determining the march's `atEnter`) was still using the unmodified
   `R_EARTH+kAuroraShellInnerM`, meaning sample points were strictly bounded to `[atEnter,atExit]`
   with atEnter sitting exactly at the 95km crossing — no sample would ever have `altM<95000`
   regardless of how far the density formula's own falloff was widened, since there'd be nothing
   below that altitude to evaluate it at. Widened the same 15km to `R_EARTH+kAuroraShellInnerM-15000`
   so the march physically reaches down to where the softened density formula now has something to
   find. **This is the same lesson as follow-up #9's original vertical fold-axis work: a march's
   SAMPLE BOUNDS and its DENSITY FORMULA are two separate mechanisms that both have to agree on
   where the phenomenon exists — softening one without the other is a no-op.**

Not yet seen in-app.

**Session 28 follow-up #18 (same day) — the widened-bounds fix from follow-up #17 was itself
incomplete: it shrank the sphere radius but left the branch threshold unchanged, creating a
genuinely broken 15km observer-altitude band (80-95km) — exactly where the user's two comparison
screenshots (~94km vs. just above it) sat.**

`tAuroraIn`'s raySphere radius was changed to `R_EARTH+kAuroraShellInnerM-15000` (80km), but the
branch condition selecting how `atEnter` gets computed was left at `obsEffH < kAuroraShellInnerM`
(95km, unchanged). For an observer between 80-95km, branch 1 (`atEnter = tAuroraIn.y`) fires — but
that branch's entire premise is "observer is below/outside the sphere," which was true when the
sphere was at 95km but is FALSE now that it's shrunk to 80km and the observer is already above that.
`raySphere` for an outward-looking ray from just outside a sphere often returns a miss (both roots
negative) rather than the "exiting from inside" root branch 1 expects — producing a negative
`atEnter`, which puts the march's first sample points BEHIND the camera. Visually this is exactly
"airglow sharply cutting the aurora" — not a blending issue, a geometry bug corrupting where the
march even starts, in a narrow but real observer-altitude band. Fixed by introducing one
`kAuroraMarchInnerM` constant (`kAuroraShellInnerM - 15000`) used consistently for BOTH the sphere
radius and the branch threshold, so they can no longer disagree. **Lesson: introducing a second
constant that's DERIVED from an existing one (here, "existing minus 15000") but only threading it
through SOME of the places the original constant was used, not all, silently creates exactly this
class of bug — grep for every use of the original constant when deriving a new one from it, don't
just fix the specific line that was visibly wrong.**

Build note: shader/executable compiled successfully; the subsequent asset-copy step failed
(locked file, likely the app running live while the user captured comparison screenshots) — not a
code issue, doesn't affect whether this fix is in the built exe.

Not yet seen in-app.

**Session 28 follow-up #19 (same day) — user reported the cut persisted even from 500km (well
above both shells) and explicitly asked for a real blend, not another edge-width tweak: "we
genuinely just need to blend the aurora and airglow together."**

Reframed the diagnosis: all the PRIOR fixes (follow-up #17/#18) made the DENSITY/ALPHA transition
at each shell edge genuinely smooth — but `auroraCurtainNoise`'s fold texture stayed at FULL
CONTRAST all the way to where `vert` (the density term) hit exactly zero. Airglow, in the same
altitude range, is a smooth, uniform Gaussian glow with no fold texture at all. So even with alpha
fading correctly, the STRUCTURED, sharply-defined curtain folds handed off directly to airglow's
featureless glow — a texture/character discontinuity, not a brightness one, and exactly why
softening density alone never read as an actual blend no matter how wide the transition zone got.

Two changes in `auroraSampleAt`:
1. **Fold contrast now fades toward flat (`mix(1.0, fold, vert)`, not raw `fold`)** — as `vert`
   approaches either edge, the curtain's own structure smooths out into a uniform glow BEFORE it
   fades away, instead of staying sharply textured until the moment it vanishes.
2. **Color also blends toward the adjacent airglow band's own color at each edge** — `innerVert`/
   `outerVert` (previously pre-multiplied into one `vert`, now kept separate so each can drive its
   own blend weight) mix `col` toward `mix(kAirglowSodiumColor, kAirglowGreenColor, 0.5)` near the
   inner edge (matching green/sodium's real 83-105km presence) and toward `kAirglowRedColor` near
   the outer edge (matching red's real 200-350km presence) — so the HUE transitions into airglow's
   characteristic color too, not just aurora's own base-to-top internal gradient stopping short and
   handing off to a totally differently-colored, unrelated glow.

**Lesson for this specific bug pattern: "smooth the density/alpha" and "blend visually" are NOT the
same fix — two independently-rendered volumetric phenomena sharing an altitude band need their
STRUCTURE (noise contrast) and COLOR to converge toward each other near the shared boundary, not
just their opacity.** Purely fading brightness to zero, however gradually, still reads as a hard
edge if what's on both sides of that fade looks nothing alike.

Not yet seen in-app.

**Session 28 follow-up #20 (same day) — user confirmed the airglow blend now reads well, but found
a NEW hard edge at exactly 80km, asking whether it was a different emissive layer or the atmosphere.
It was the aurora's own inner boundary again — follow-up #19's "symmetric 15km widening" (80-110km)
never actually removed the hard floor, it only relocated it from 95km to 80km.**

Root cause, precisely: `smoothstep(edge0, edge1, x)` is mathematically EXACTLY zero at and below
`edge0` regardless of how far `edge1` is from it — widening a smoothstep's span moves where its
floor sits, it can never eliminate the floor itself. Every prior "widen the transition" fix in this
sub-thread (follow-up #17, #19) was still built on smoothstep, so each one just relocated the wall
rather than removing it — this session's actual fix.

Replaced both `innerVert` and `outerVert` (in `auroraSampleAt`) with SIGMOID functions
(`1/(1+exp(∓(altM-center)/falloff))`) instead of smoothstep. A sigmoid asymptotically approaches 0
and 1 without ever exactly reaching either — there is no altitude at which it's mathematically zero,
so there's no floor left to relocate. `kAuroraInnerFalloffM=7500`/`kAuroraOuterFalloffM=15000` set
the transition's rough width (~4x this value spans ~12%-88%), playing the same role smoothstep's
span used to.

This alone wasn't sufficient, though — the march's own SAMPLE BOUNDS (raySphere radii in `main()`)
were still fixed at the old hard boundaries, and sample points are strictly confined to
`[atEnter,atExit]` regardless of what the density formula would compute further out. A sigmoid's
tail is still practically negligible past some point (the existing `vert<=0.001` early-out still
culls it), but that cull point needs to be REACHABLE by the march, not pre-clipped by bounds that
still assume a hard floor exists. Extended both:
- **Inner**: `kAuroraMarchInnerM = kAuroraShellInnerM - 55000` (~40km) — the inner sigmoid drops
  below the 0.001 threshold around 43km, so the march bound needed to sit below that, not at it.
- **Outer**: `kAuroraMarchOuterM = kAuroraShellOuterM + 110000` (~410km) — mirrored fix, proactive
  rather than reported: the outer sigmoid has the identical issue around 300km that the inner one
  had at 95km, just not yet hit in testing. Fixed both edges together rather than waiting for a
  separate bug report on the outer one.
- Both new constants drive BOTH the raySphere radius AND the corresponding branch threshold
  together (same "must match or create a broken observer-altitude band" lesson as follow-up #18).

**Lesson: `smoothstep` cannot produce a soft edge with no floor, no matter how it's parameterized —
it's the wrong tool whenever "genuinely no hard edge, ever" is the actual requirement (as opposed to
"a wide soft edge"), and reaching for "just widen it further" repeats the same mistake with a
smaller error each time rather than fixing it. Should have looked for the parametrically-different
tool (sigmoid, or any asymptotic curve) after the SECOND time widening didn't work, not stayed on
the third attempt with the same function.**

**Confirmed working in-app** — user: "the sigmoid change genuinely worked extremely well! Not only
is the blend perfect with the aurora on the bottom, but I am now seeing a lot more pink detail at
the top of the auroral columns." The pink/magenta detail is an expected, positive side effect of the
SAME session's outer-edge fix (`kAuroraMarchOuterM`, extended proactively alongside the inner one):
`col`'s altitude gradient mixes toward `kAuroraTopColor` (red/magenta) as altitude approaches
`kAuroraShellOuterM`, but the old hard-floored outer edge (smoothstep, floor at exactly 300km) cut
the march off before much of that upper, reddest portion of the gradient was ever sampled — the
extended march bounds mean the march now actually reaches high enough to render it. Session 28
Phase E aurora work (C16) is now visually confirmed solid at both shell edges — inner
(blend-with-airglow) and outer (color gradient) — pending only further open-ended tuning, not more
edge-geometry bug hunting.

**Session 28 follow-up #21 (2026-07-16) — two enhancement requests on the now-solid aurora, both in
`sat_sky.frag`:**

1. **Per-column elevation variation.** Every aurora column previously spanned the full
   `kAuroraShellInnerM`→`kAuroraShellOuterM` range (`vert`/`innerVert`/`outerVert` in
   `auroraSampleAt` don't vary with colat/az), so every fold showed the identical complete
   green-to-pink gradient — user wanted some columns low-altitude only, some high-altitude only,
   some full-span. Added a NEW, separate windowing factor (`columnWindow`) rather than touching
   `vert`/`innerVert`/`outerVert` themselves: those two still anchor the color blend and the
   airglow hand-off to the TRUE shell bounds (needed for follow-up #20's fix to stay correct), so
   disturbing them would have broken that. Sampled two decorrelated low-frequency `warpPerlin3`
   values (`colA`/`colB`, `kAuroraColumnFreq=9.0`, well below the fold texture's own tangent/radial
   frequencies) as a function of `(colat, az)` ONLY — no altitude, no time, so it's constant up the
   full height of a given column and doesn't flicker frame to frame. Sorting the two samples into
   lo/hi (`colLoFrac=min-0.08`, `colHiFrac=max+0.08`) rather than deriving a window from one
   center+halfwidth value falls out naturally into the requested spread: when the two samples land
   close together the window is narrow (low or high depending on where), when they land far apart
   (near 0 and near 1) the window covers nearly the whole shell. Converted to altitude via
   `mix(kAuroraShellInnerM, kAuroraShellOuterM, frac)`, gated by two more sigmoids
   (`kAuroraColumnFalloffM=12000`, same asymptotic-no-floor reasoning as follow-up #20) multiplied
   into the final return alongside `vert`. A "low-altitude-only" column now naturally shows only
   the green base color (it fades to zero via `columnWindow` before `col`'s gradient ever reaches
   the pink/red end) with no extra color-logic needed — the visibility windowing alone produces the
   height-based color variation the user was asking for.

2. **Organic shimmer evolution.** `auroraCurtainNoise`'s time term (`cloud.auroraShimmerRate`) was
   added directly onto the azimuthal coordinate (`az * kAuroraTangentFreq + t * shimmerRate`) — a
   pure linear translation. This had already been moved once before (originally on the altitude
   axis, causing "columns flicker top to bottom," fixed by moving to azimuth), but a straight
   additive shift is mechanical regardless of which axis carries it; user now reported it read as
   "a spinning texture through the aurora itself, moving east to west" — correctly identified as
   still just rigid translation, not organic motion. Replaced with the domain-warp technique already
   established for cloud shape (`cloudWarpOffset`): a separate `warpPerlin3` evaluation
   (`shimmerPhase`, inputs `colat`/`altM`/`t*shimmerRate`, scaled ×2.5) now produces the phase
   offset fed into the main fold-noise sample, instead of `t*rate` being added directly. Because the
   phase itself comes from a noise field rather than linear time, it evolves non-monotonically, and
   because `colat`/`altM` feed the SAME warp evaluation the phase varies smoothly across the curtain
   rather than shifting every fold by an identical amount in lockstep — reads as folds morphing
   rather than the whole sheet sliding sideways at constant velocity.

Both changes build clean (shader compiles, exe links); one `cmake --build` attempt hit the familiar
transient asset-copy lock on `assets/sound/music/*.mp3` (not a code issue — individual file copies
succeeded immediately after, and a subsequent full rebuild completed cleanly with no changes
needed). Not yet seen in-app.

**Session 28 follow-up #22 (2026-07-16) — user confirmed the two follow-up #21 changes worked well
and declared aurora "done for now."** Promoted the current in-app-tuned `build/Debug/settings.json`
values to `SatelliteSim.h` member-initializer defaults, same pattern as follow-up #9. Notable swings
(debug tuning since follow-up #9, not new bugs): `cloudMarchSteps` 4→75.6 (quality raised
substantially since the perf-budget work), `cloudDensity` 10→5.48, `cloudCoverage` 0.693→0.614,
`cloudHgG` 0.156→0.274, `lightPollutionGain` 38.6→6.14, `extinctionCoeff` 0.368→0.399,
`auroraShimmerRate` 0.025→0.00175 (much slower now that follow-up #21's warp-phase evolution reads
as organic rather than a translating texture — a lower rate no longer needs to fight the old
mechanical slide to look alive), `auroraCoverageDriftRate` 0.000395→0.00194,
`auroraCoverageFreq` 0.649→0.426, `auroraCoverageAzFreq` 4.026→4.289, `auroraGain` 0.0785→0.1,
`auroraGroundGain` 0.00439→0.00746, `stormStrength` 0.355→0.320. Left untouched (matched within
rounding): `brightnessScale`, `daySuppression`, `mirrorBoost`, `visThresh`, `highlightFlare`,
`moonSuppression`, all four airglow gains, `cloudBaseAltM`/`cloudTopAltM`/`cloudDriftRate`,
`cloudSunGain`, cirrus wind/stretch, `viewSamplesMin`/`Max`, `lightSamples`, ocean octave/sample
counts, `moonGain`, `auroraCloudGain`. Build clean. **Aurora (C16) is closed for now** — Phase E
is functionally complete except C14 (Anvil), still deliberately deferred with no dependency on
C15/C16.

### 2026-07-13 (session 25)
- **Terrain night ambient + moonlight — first two of four unscoped terrain light sources**
  (ambient/lunar/satellite/aurora were all missing; satellite and aurora remain unscoped). Both
  land in `sat_sky.frag`'s terrain block, adjacent to the existing sun/`skyAmbientTerrain` code
  (~line 1295-1379), and both are additive — they replace nothing.
  - **Night ambient:** `nightColor * 0.12` (city-light emission) was the only thing keeping
    unlit terrain off pure black; there was no real reflected-ambient term. Added
    `dayColor * kNightAmbientBase * cloud.nightAmbientGain` — lights true albedo, not the
    night-lights texture, so rural terrain now gets a dim cool-toned floor independent of city
    proximity. `kNightAmbientBase = vec3(0.12, 0.15, 0.24)` is a hardcoded physical-magnitude
    constant (matches the `kNightFloor`/`kCityCompressK` convention elsewhere in this file);
    `nightAmbientGain` is the UBO-tunable multiplier. Always present (not gated by `dayFrac` or
    moon state) — real night-sky brightness (starlight/airglow/zodiacal) doesn't depend on the moon.
  - **Moonlight:** mirrors the *sun's* own direct-light pattern in this same block
    (`shadingN·dir` Lambertian + `geoSunDot`-style geographic horizon gate via
    `dot(normalize(hitPt), moonDir)`) rather than `cloud_march.comp`'s self-shadow/phase model —
    terrain has no volumetric self-occlusion to fake, and `hitPt`/`shadingN`/`sunDir` already
    share one frame with no ECEF conversion needed (confirmed by reading the existing `geoSunDot`
    comment), so the sun scaffolding was the closer, cheaper fit. `moonDirENU.w` (illuminated
    fraction) reused directly, same as the moon-disc and cloud code already do.
  - **Cloud/terrain coherence tie-in (explicit user ask):** `cloud_march.comp`'s `moonContrib` used
    a hardcoded `0.015` scalar with no way to tune it without a shader edit. Replaced with the new
    shared `cloud.moonGain` UBO field, which the new terrain moonlight term also reads — one
    slider now controls moonlit terrain and moonlit clouds together, so they can't drift apart in
    brightness. Default `0.015` preserves the prior cloud look exactly.
  - `GpuCloudParams` grew 224→240 bytes: `nightAmbientGain`, `moonGain`, `pad1`, `pad2` (2 real +
    2 reserved, matching the established padded-growth pattern). Struct is duplicated in
    `cloud_march.comp` (no `#include` in this codebase) — updated both copies; a mismatch here is
    silent garbage, not a compile error, since it's just a UBO layout. New settings sliders
    "Night ambient" / "Moon gain" added (ids 24/25, `cloudBufs` resized 24→26); persisted to
    `settings.json` under `clouds.terrain_night_ambient` / `clouds.moon_gain`.
  - Build clean (shaders + C++). Not yet visually tuned by the user — defaults are physically-
    motivated guesses (`terrainNightAmbient=0.02`, `moonGain=0.015` inherited from the old cloud
    constant), expect slider adjustment once seen at night in-app.
  - **Bugfix caught while extending this same slider array pattern:** the prior sliders-array
    resize (24→26 for the two new cloud sliders above) missed 3 fixed-size hover/drag arrays —
    `hovCloudMinus`/`hovCloudPlus`/`draggingCloud` in `SatelliteSim.h` were still `[24]`, an
    out-of-bounds read/write for the new idx-24/25 sliders. Resized to `[26]`. Same class of bug
    almost repeated moments later for the photometry sliders below (`hovPhotoMinus`/`hovPhotoPlus`/
    `draggingPhoto` are separate `[N]` arrays parallel to the settings-window slider list, not
    auto-sized by the array-of-struct + range-for loop the sliders themselves use) — caught before
    building this time. Any future slider addition to either panel must grep for all of these
    parallel hover/drag arrays, not just the visible `*Bufs`/params array.
- **Satellite moonlight sky-brightness suppression (same session, `sat_flare.comp`).** Extends the
  cloud/terrain moonlight coherence work above to satellite visibility: satellites were already
  dimmed by daylight sky background (`dayBright × daySuppression`) and by city light pollution
  (`pc.lightPollution`, a single CPU-computed scalar from `earthNightTex` at the observer's own
  position — correctly varies by where the observer stands, but applies uniformly across the whole
  sky regardless of which direction a satellite appears in), but had **no moonlight term at all**.
  Added `moonBright` (same squared-elevation-ramp shape as `dayBright`, scaled by phase illumination
  `(1-dot(sunDirECI,moonDirECI))/2`) into the *same* suppression denominator as `dayBright` — both
  are the same physical mechanism (sky background raised, faint objects lose contrast), so they sum
  before dividing, unlike the multiplicative `lightPollution` term which is a separate effect.
  `moonSuppression` (user-tuned to 4.0 after seeing it in-app — daySuppression's ~1500 for scale
  reference, the two aren't directly comparable since moonBright's ramp/illumination inputs differ
  from dayBright's) is the new tunable, mirroring `daySuppression`'s role at realistically much
  lower magnitude. `SatFlarePC` grew 104→128 bytes (`moonSuppression`, `pad0`, `moonDirECI`, `pad1`
  — vec3 needs 16-byte alignment, hence the pads); new "Moon suppress" photometry slider (idx 5);
  persisted to `settings.json` under `photometry.moon_suppression`. Build clean. User confirmed both
  this and `cloud.moonGain`'s default (0.015, unchanged) look right in-app.
  - **Explicitly deferred (user-scoped for later):** both `dayBright` and `moonBright` are uniform
    across the sky — same simplification, not direction-dependent. A real light-pollution model
    needs a light dome (brighter near the horizon toward nearby cities, fainter away/at zenith),
    which would need a small azimuth-binned array sampled from `earthNightTex` around the observer,
    built CPU-side before the compute dispatch (can't reuse `glowBuf` itself for this — it's an
    *output* of `sat_flare.comp`, built from the very satellites this would need to dim, so using
    it as an input is circular). Moonlight's near-disc sky-brightening halo (real moonlight scatters
    more strongly close to the moon) was folded into this same deferred scope rather than built now.
  - **Follow-up (same session): stars had no moonlight term either.** `updateStars()` (CPU-side,
    `SatelliteSim.cpp`) already dimmed stars for light pollution (`kStarPollutionMaxDim=0.85` cap)
    but had nothing for moonlight. Added `moonBrightStar` — same elevation-ramp shape as the
    satellite/`sat_flare.comp` version, but reuses `moonDirENU.w` (illuminated fraction, already
    computed that frame in `updatePositions()`, called immediately before `updateStars()`) instead
    of re-deriving it from `sunDirECI`/`moonDirECI`. Applied as a fixed multiplicative cap
    (`kStarMoonMaxDim=0.6`), **not** wired to the `moonSuppression` slider — stars' existing
    day/pollution dimming (`nightFactor`, `kStarPollutionMaxDim`) are both fixed formulas with no
    settings-window knob, so this follows that established star-specific pattern rather than the
    satellite side's explicit-gain-slider pattern; the two subsystems use different suppression
    *shapes* (multiplicative cap vs. additive-denominator) so the raw slider value isn't portable
    between them anyway. Build clean.
- **Remaining unscoped terrain light sources:** satellite-reflected light (satellites as light
  sources illuminating terrain/clouds, not the moonlight-suppression work above, which is about
  satellite *visibility*) and aurora ground-cast light were raised in the same conversation but
  explicitly deferred — not designed, not started.

### 2026-07-13 (session 26)
- **Terrain night ambient removed — user feedback after seeing it in-app: it should stick to 0,
  slider removed.** Reverted `sat_sky.frag`'s terrain block to its pre-session-25 form (plain
  `nightColor * 0.12`, no `nightAmbientTerrain`/`kNightAmbientBase` addition). `GpuCloudParams`'
  `nightAmbientGain` slot reverted to reserved pad in all three copies (`SatelliteSim.h`,
  `sat_sky.frag`, `cloud_march.comp`) rather than shrinking the struct — same "was padN" convention
  established when `cloudShadowFactor` was removed (session 23). Named the reclaimed slot `pad3` in
  all three, since `pad0`/`pad1`/`pad2` were already taken by earlier reclaims in this same struct
  (a duplicate-`pad0` name collision was caught by the shader compiler and the equivalent C++
  duplicate-member error would have hit next — same struct, same slot, three copies to keep in
  sync). Removed the "Night ambient" settings slider (was idx 24), renumbered "Moon gain" to
  idx 24, shrank `cloudBufs`/`hovCloudMinus`/`hovCloudPlus`/`draggingCloud` back to 25. Removed
  `terrainNightAmbient` member and its `settings.json` persistence. Build clean.
- **`kStarMoonMaxDim` tuned by the user directly in the IDE, 0.6→0.9** (`SatelliteSim.cpp`,
  `updateStars()`) — full moon dims naked-eye stars more aggressively than the initial guess, still
  short of 1.0 so the very brightest (Venus/Jupiter/Sirius-class) still show through.
- **Both `moonSuppression=4.0` (satellites) and `cloud.moonGain=0.015` (terrain+clouds) confirmed
  good** — no changes needed to either this session.
- **Directional light-pollution dome, implemented (the follow-up scoped at the end of session
  25).** Replaces the single "city brightness at the observer's own lat/lon" scalar (correct about
  moving with the observer, wrong about being uniform across the whole sky) with an 8-azimuth-sector
  dome — brighter toward nearby cities, fainter elsewhere, and independently faded toward zenith vs.
  horizon. Full design + formulas documented in `CLAUDE.md`'s new "Subsystem: Light Pollution Dome"
  section rather than duplicated here — key points:
  - New `SatelliteSim::updateLightPollutionDome()` (CPU, called each frame right before
    `updateStars()`): 8 sectors × 3 sample radii (8/20/45 km) via flat-Earth tangent-plane lat/lon
    offset into `earthNightCpu`, combined per-sector via **weighted max** (a single nearby bright
    city should dominate that direction, not get averaged down by darker samples at other radii in
    the same sector — a plain weighted average would defeat the point of sampling multiple radii).
    Reuses the exact same response-curve/altitude-falloff constants as the old scalar; only the
    sampling geometry is new.
  - Sector convention (45° bearing clockwise from North) deliberately matches `sat_flare.comp`'s
    *existing* `GlowBuf` `sectorBright`/`azBin` scheme exactly, rather than introducing a
    differently-sized second directional binning in the same shader.
  - New `lightDomeBuf` SSBO (binding 3 in the sat_flare.comp descriptor set — `bindings[]`/`ps`/
    `writes[]` arrays all grew 3→4), host-visible/mapped, 8 floats, same "CPU writes this frame, GPU
    reads it later this same frame, single frame in flight, no barrier" pattern as
    `reflectorTargetsBuf`. `updateStars()` reads the CPU-side `lightDomeAz[]` array directly — no
    upload round-trip needed for stars, only satellites need the GPU buffer.
  - `pc.lightPollution` removed from `SatFlarePC` (reverted to `pad2` — another `padN`-collision
    near-miss, same lesson as the terrain-ambient removal above: this struct now has reserved slots
    at 3 different points from 3 different removed features, easy to reuse a name by accident).
  - Old flat `lightPollution` member removed entirely from `SatelliteSim.h` (dead once both
    consumers moved to the dome).
  - Build clean.
  - **Deferred, not built:** the elevation falloff is a fixed analytic curve, not a true 2D
    (azimuth × elevation) sampled dome — judged not worth real atmospheric-scattering-height
    modeling over the fixed-curve approximation. Not re-scoped as a "next step"; revisit only if
    the fixed curve visibly looks wrong in-app.
- **Light pollution dome follow-up, same session: user reported the effect was basically invisible
  in-app.** Root causes (not one bug, three compounding factors) and fixes:
  1. **No tunable gain existed at all** (by design, matching the old scalar's precedent) — added
     `lightPollutionGain` (default 1.0, settings slider "Pollution gain"), applied once at the
     source in `updateLightPollutionDome()` so both consumers stay coherent by construction.
  2. **Missing near-field sample:** the 3 radii started at 8 km, so standing in/near a small or
     isolated city (common — the default 67°S 67°W spawn point and most manually-picked spots
     aren't inside a sprawling metro) could miss the pollution source if the immediate
     surroundings within 8 km were already dark. Added a 2 km near sample (4 radii total:
     2/8/20/45 km) — the direct analog of the old scalar's distance-0 sample.
  3. **Elevation falloff too steep:** `0.15/(sin(el)+0.15)` crushed the effect to near-zero above
     ~20° elevation — most satellites/stars a user actually looks at aren't sitting right on the
     horizon. Softened to `0.35/(sin(el)+0.35)` in both `sat_flare.comp` and `updateStars()`
     (kept identical between the two, as before). Build clean.
- **Light pollution dome, second follow-up (same session): still blocky over Europe, and gain=500
  looked identical to gain=5** (user had edited the slider's own `vmax` to 500 directly in the IDE
  trying to compensate). Two real bugs, not a tuning problem:
  1. **Saturation bug:** `lightDomeAz[sec]` was clamped to `[0,1]` *before* `elevFalloff` (≤1 off
     the horizon) got applied downstream — so gain above the point where it first saturated
     (~5) could push the pre-elevFalloff value arbitrarily higher with zero visible effect, since
     it was already being multiplied by a fraction afterward. Fixed by removing the premature
     clamp at the source; the final `domeVal = clamp(domeAz * elevFalloff, 0, 1)` downstream (in
     both `sat_flare.comp` and `updateStars()`) is the only clamp now, so high gain can actually
     compensate for elevFalloff's reduction at non-horizon angles. Tightened the slider's `vmax`
     back from the user's temporary 500 to 10 — full saturation at every elevation now happens
     around gain≈4-5, so 500 was 100× oversized for the useful range and made the slider nearly
     unusable for fine control.
  2. **Blocky sectors:** 8 hard-edged 45° wedges, each from a handful of point-samples along one
     bearing ray — crossing a sector boundary was a discrete jump, visible over wide, fairly
     uniform bright regions (flying over Europe). Fixed two ways together: bumped to 16 sectors
     (22.5° each), and switched both consumers from hard `azBin` lookup to interpolating between
     the two nearest sector *centers* (`mix(lightDome[sec0], lightDome[sec1], frac)`) — the
     interpolation is the actual fix for "blocky transitions" (finer sectors alone would still
     have hard edges, just more of them). No longer needs to match `GlowBuf`'s own 8-sector
     `azBin` scheme (that reuse was for convenience, not a hard requirement) — decoupled.
  Build clean.
- **Light pollution dome, third follow-up + new atmospheric extinction feature (same session):**
  user reported still-blocky transitions when flying over a large uniform region (Europe) even
  after the 16-sector interpolation fix, and separately asked what horizon-elevation atmospheric
  attenuation existed for satellites/stars — answer: none did.
  - **Dome smoothing:** the 16-sector interpolation smooths *within* a sector's span but doesn't
    reduce how different two neighboring *raw* sector values are — each sector is one bearing ray,
    and a real city edge doesn't line up with 22.5° boundaries, so adjacent sectors could still
    swing hard. Added a 5-tap circular blur (`[0.1,0.2,0.4,0.2,0.1]`, ~±45°) over the raw
    `lightDomeAz[]` values before storing — this is the actual fix for "unsubtle pops when
    panning," not the interpolation or sector count.
  - **New: atmospheric extinction (airmass).** Kasten & Young 1989 approximation, identical formula
    in `sat_flare.comp` and `updateStars()` (a star and satellite at the same elevation must dim
    equally — this is real light transmission, not a stylized knob). Multiplies `effectFlare`/star
    intensity directly, `atmFrac`-gated (no atmospheric column from orbit). New `extinctionCoeff`
    tunable (default 0.25 mag/airmass, settings slider "Extinction") reuses `SatFlarePC`'s `pad2`
    slot — no struct growth. This gives the pollution dome's directional noise a smooth baseline to
    sit on top of, rather than being the *only* source of horizon-vs-zenith brightness variation
    (part of why the dome felt unsubtle before this). Full design in `CLAUDE.md`'s new "Subsystem:
    Atmospheric Extinction". Build clean.
  - Noted in passing: the user has been tuning `kStarPollutionMaxDim` (now 0.99, was 0.85) and
    slider ranges (`Moon suppress`/`Pollution gain` `vmax`) directly in the IDE between rounds —
    left those as found, only touched what was needed for these fixes.

### 2026-07-13 (session 23)
- **Half-resolution cloud compute pass — biggest architecture change to `sat_sky.frag` to date.**
  User's core ask: clouds are soft/low-frequency, don't need full-res per-pixel marching; move the
  march to a cheaper resolution and composite/upsample. Full design work happened in Plan mode
  (two research passes — one direct codebase exploration, one independent design-validation pass
  that caught two real bugs before any code was written) and the approved plan is preserved at
  the session's plan file (not in this repo). Implementation:
  - **New `shaders/cloud_march.comp`**, dispatched once per frame in `recordCompute()` (before the
    render pass — required, since compute dispatches can't happen inside one) at half
    `ctx.swapExtent` in each dimension (1/4 the pixels). `cloudMarch()`/`cirrusMarch()` (plus
    `cirrusWindAngleAt`/`cirrusDomainWarp`, exclusive to cirrus) **moved** here wholesale, deleted
    from `sat_sky.frag`. A handful of shared primitives (`raySphere`, `phaseM`, `phaseCloud`,
    `optDepth`, `remap`, `rotateZ`, `cityBrightness`, `cloudDensity`, `cloudWarpOffset`, the
    `warpPerlin3` noise chain) are **duplicated**, not moved — this codebase has no `#include`
    mechanism, and every one of these is also used elsewhere in `sat_sky.frag` (verified by
    grepping every call site before deleting anything — `cloudDensity`/`cloudWarpOffset` looked
    cloud-exclusive but are also used by `cloudShadowFactor`, the ground/ocean cloud-shadow pass,
    which stays in the fragment shader).
  - **Restructured, not just relocated:** the original functions mutated an `inout vec3 color`.
    The compute-shader copies (`cloudMarchCS`/`cirrusMarchCS`) instead **return** their affine
    composite as an `(A, B)` pair (`color_out = color_in*A + B`), so cirrus-then-cloud combine
    algebraically into one `(A_total, B_total)` — exact, not an approximation:
    `A_total = A_cirrus*A_cloud`, `B_total = B_cirrus*A_cloud + B_cloud`.
  - **Two `RGBA16_SFLOAT` output images** (`cloudMarchTargetA/B`, new members in
    `SatelliteSim.h`): Target A = `B_total` (additive radiance) + `tCloudOcclude` (alpha, meters,
    -1 = none — only `cloudMarch` sets this, only when ≥90% opaque). Target B = `A_total`
    (multiplicative attenuation, kept full-color — reducing to scalar would desaturate sunset edge
    tinting) + `cloudBlock` (alpha, `dot(cloudTOut, 1/3)`, feeds the existing post-tonemap sun-
    disc-dimming term at `sat_sky.frag` line ~1746 exactly as the old `cloudTFinal` local did).
  - **No terrain data in the compute shader** — it always marches the shell's full potential
    extent (no `earthElevTex`/`earthSpecTex`, no terrain march, ~320 steps avoided). `sat_sky.frag`
    does a post-hoc correction instead: if its own accurate `tSurface` is closer than the sampled
    `tCloudOcclude`, suppress the whole cloud contribution for that pixel. **Known, accepted
    approximation** (not a bug): exact for "terrain fully blocks an opaque cloud," not for a
    mid-shell partial truncation (a mountain ridge poking into the shell — `cloudBaseAltM` default
    6000m is well below Everest's 9000m) or for wispy/sub-90%-opacity cloud (which never sets
    `tCloudOcclude` at all). Clouds can visibly extend slightly past a nearby ridge silhouette at
    ground level; largely irrelevant for the primary orbit/LEO motivation.
  - **CPU-side sequencing fix required**: the `CloudParams` UBO fill and the `obsTerrainH` CPU
    elevation lookup both used to happen in `recordDraw()` — too late, since the new compute
    dispatch needs them and runs in `recordCompute()`, which executes first. Both blocks
    **relocated** into `recordCompute()` (placed before the `activeSatCount == 0` early-return, so
    clouds don't silently stop rendering whenever no satellites are active). New `CloudMarchPC`
    push-constant struct (128 bytes, mirrors `SatDrawPC`'s shape) carries `obsEffH =
    max(obsTerrainH, obsHeightOffset)` explicitly — `pc.obsECEFDir.w` is `obsHeightOffset` *only*,
    a stale comment elsewhere claiming otherwise was not trusted.
  - **Two explicit `ctx.imageBarrier` calls per target, twice per frame** (before AND after the
    dispatch — four barriers total): storage-image writes require `VK_IMAGE_LAYOUT_GENERAL`,
    sampling requires `SHADER_READ_ONLY_OPTIMAL`, and the render pass's existing
    `VK_SUBPASS_EXTERNAL` dependency (the one that already covers `glowBuf`) has no layout fields
    and cannot perform this transition — confirmed by reading `VulkanContext.cpp`'s subpass
    dependency struct before relying on it.
  - New descriptor set (`cloudMarchDescLayout/Pool/Set`, 7 bindings: `earthCloudsTex`,
    `cloudNoiseTex`, `earthNightTex`, `noiseTex`, `CloudParams` UBO, + 2 storage-image targets) and
    2 new bindings (10/11) on the existing `skyDescSet`. `createCloudMarchResources` (images) must
    run before `createGlowResources` (writes those 2 bindings); `createCloudMarchDescriptors`
    (needs `cloudParamsBuf`) must run after it — see the `init()` ordering comment.
  - `onResize` recreates both images at the new half-extent and patches both descriptor sets
    (2 targeted `vkUpdateDescriptorSets` calls) — the only swapchain-size-dependent images this
    class owns.
  - Local workgroup size `16×16` (matches `game_of_life.comp`'s house convention for 2D per-pixel
    compute, not the 1D `64` satellite shaders or the 3D `8×8×8` noise bake), dispatch
    `((halfW+15)/16, (halfH+15)/16, 1)`.
  - Both new shader files compiled clean standalone (`glslc`) before the full C++ integration was
    written, and the full `cmake --build build` succeeded on the first attempt after all pieces
    were wired up — no compile-error iteration needed this session.
  - Not run interactively — per `CLAUDE.md`, the user verifies. Primary things to check: FPS
    improvement from orbit/LEO and at a dense ground-level cloud deck; satellite/star occlusion
    behind opaque cloud still works (edge softness from bilinear upsampling is expected and
    accepted, not a bug); window resize doesn't crash or leave stale cloud data; the accepted
    mountain-silhouette approximation isn't jarring in practice.

- **Follow-up bug found by the user in testing: cloud lighting/scattering visibly bled through
  terrain.** Root cause was worse than the documented "Known limitation" above — the terrain-
  suppression gate used `tCloudOcclude` (Target A's alpha), which `cloudMarchCS` only sets when
  the cloud is ≥90% opaque (that threshold exists for the SEPARATE satellite-occlusion use case,
  to stop wispy cloud from snapping satellites invisible). Since most cloud coverage isn't ≥90%
  opaque everywhere, the suppression check almost never fired — nearly all non-solid cloud
  rendered through terrain regardless of depth, not just the documented rare mid-shell case.
  **Fixed:** both `cloudMarchCS` and `cirrusMarchCS` now also return an always-valid `tEnterOut`
  (set whenever the march genuinely runs, independent of opacity). The compute shader combines
  both layers' entries via `min()` (nearest layer) and stores it in Target B's alpha — which
  freed up Target A's alpha to keep the opacity-gated `tCloudOcclude` for its original satellite-
  depth purpose. `cloudBlock` (the post-tonemap sun-dimming scalar that used to live in Target
  B's alpha) is now derived from Target B's RGB (`dot(A_total, vec3(1/3))`) instead — a minor,
  accepted approximation (A_total already tracks combined opacity closely) that was the only way
  to free the channel within the existing 2-target budget. `sat_sky.frag`'s terrain-suppression
  check now uses this always-valid distance instead.

- **Follow-up: performance was unchanged (Release build tested) — root cause was a separate,
  never-touched function, `cloudShadowFactor()`, now removed.** After the terrain-bleed fix, the
  user tested Release-build FPS across several scenes: surface ~20fps, LEO horizon ~40fps, LEO
  looking down at clouds ~28fps, satellites/empty-space 80-120fps. Critically, **surface FPS
  (~20fps) was unchanged across the ENTIRE session** — before any cloud-march optimization, after
  wiring `lightSteps`, after distance-gating the shadow cone, and after moving the march to
  half-res compute. That convergent evidence (four different fixes, zero surface FPS movement)
  pointed away from `cloudMarch`/`cirrusMarch` entirely. Setting `coverage=0` confirmed clouds
  were still the dominant surface cost regardless (20fps → 40fps ocean / 58fps terrain) — so the
  real cost had to be in cloud-related code NONE of this session's work had touched:
  `cloudShadowFactor()`, which projects cloud shadows onto terrain/ocean, runs at full resolution
  on essentially every terrain/ocean pixel (most of the screen at ground level), with its own
  step-count march (`cloud.shadowSteps`, unrelated to `cloudMarch`'s `marchSteps`/`lightSteps`).
  **User's call: remove it outright** — cloud shadowing on terrain/ocean isn't currently in use,
  so this dominant remaining cost is simply gone rather than optimized. `directSun` (terrain sun
  lighting) and the terrain specular highlight no longer multiply by it. Its `CloudParams` UBO
  slot (`shadowSteps`) reverted to `pad0` (it was `pad0` originally, before C-something wired up
  the now-removed slider) rather than leaving a dead-named field — matches this file's established
  pad-slot convention. The `CLOUD_ISOLATE_COLH`/`CLOUD_ISOLATE_SHADOW` debug switches and the
  `kShadowFloorLo/Hi`/`kShadowHardCutoff`/`kShadowStepPathCapM` constants — all exclusively used
  by the removed function or already-orphaned by the half-res move earlier this session — went
  with it. "Shadow steps" slider removed from settings UI; `cloudBufs`/`hovCloudMinus/Plus`/
  `draggingCloud` shrunk `[19]`→`[18]`, all subsequent slider indices renumbered down by one.
  Both Debug and Release builds clean. **Not yet re-tested by the user for the FPS impact of this
  removal** — that's the natural next data point before scoping further optimization work.

### 2026-07-13 (session 24)
- **Terrain/ocean/atmosphere perf follow-up**, after the `cloudShadowFactor()` removal in session
  23 confirmed real gains (20fps→30fps ocean/34fps terrain at ground, Release build) but fell short
  of the user's 60fps@1080p target. FPS scaling almost linearly with pixel count (a smaller default
  window hits 50s fps) confirmed the app is now purely GPU-bound on full-res per-pixel shader work
  — clouds are no longer the bottleneck. Went through Plan mode again (direct exploration + an
  independent validation pass) before touching code, per this session's established pattern.
  - **Found and fixed a real regression**: the terrain march's altitude-scaled step count
    (`sat_sky.frag` ~874) was `int kN = int(mix(320.0, 320.0, clamp(obsEffH/800000.0,0,1)));` — a
    literal no-op that always paid the LEO-tuned 320-step budget even at ground level, directly
    contradicting its own comment ("196 at ground... up to 320 at LEO"). Restored to
    `mix(196.0, 320.0, ...)`. One line, no downside, matches documented intent exactly.
  - **Bigger find from the validation pass**: the main atmosphere scattering loop
    (`N_VIEW=124`/`N_LIGHT=12`, `sat_sky.frag` ~263) runs unconditionally on **every pixel of every
    frame** — terrain, ocean, cloud, satellite, or empty space — before any surface-specific work,
    with `optDepth()`'s inner `N_LIGHT` march reused at 4 call sites (main loop, terrain lighting,
    ocean lighting, and *inside* the ocean's 6-sample reflection loop). No transmittance
    precomputation exists anywhere. This better explains the benchmark data than terrain/ocean
    alone: satellites (80fps) and empty space (120fps) have zero terrain/ocean work yet neither is
    near a plausible GPU ceiling — that gap is this loop.
  - **Methodology, not a rewrite yet**: rather than build a transmittance LUT speculatively (real
    architecture work — new bake pass, new texture/binding, 4 call sites), made `N_VIEW`/`N_LIGHT`
    UBO-tunable sliders first, same "make it tunable, let the user test on real hardware before
    investing in structural rewrite" pattern already used twice this session. The LUT is the
    natural next follow-up **only if** the user's slider test shows `lightSamples` is a big share
    of the cost — not started yet.
  - **Ocean quality knobs added the same way**: `seaMap`'s octave count (3, called up to 10x per
    ocean pixel by `heightMapTracing`'s secant refinement), `seaMapDetail`'s octave count (5, used
    by `getNormal`), and the sky-reflection loop's sample count (`N_REFL=6`) were all hardcoded
    compile-time loop bounds with zero user control — now three more sliders. Considered and
    rejected a half-res buffer specifically for the reflection color (genuinely low-frequency,
    unlike wave height/normal) — the ocean mask is an irregular per-pixel silhouette, not a
    contiguous full-screen region like sky/clouds, so a real half-res buffer needs its own
    mask/reprojection plumbing; a tunable sample count captures nearly the same win far more cheaply.
  - **Left the terrain hit's 12-step binary search alone** — confirmed it runs once per terrain-hit
    pixel (not once per coarse-march iteration), ~24 texture fetches vs. the ~400-1280 the coarse
    march already paid on the same pixel to find that hit — under 10% of the march's own cost,
    directly controls silhouette/normal precision, not worth the risk for the expected gain.
  - **`GpuCloudParams` grew 208→224 bytes**: `pad2`/`pad3` renamed to `viewSamples`/`lightSamples`
    (defaults 124/12, exactly matching prior fixed behavior); a new trailing vec4 added
    (`oceanSeaOctaves`=3, `oceanDetailOctaves`=5, `oceanReflSamples`=6, `pad5` reserved). Both GLSL
    copies (`sat_sky.frag` binding 9, `cloud_march.comp` binding 4 — same underlying buffer,
    independent struct declarations) updated in lockstep; the compute shader's copies of the 5 new
    fields are unused there (ocean/main-atmosphere-loop quality is a `sat_sky.frag`-only concern)
    but must exist for byte-offset parity. 5 new settings sliders added to the existing "Clouds"
    section (not a separate header — kept the diff minimal); `cloudBufs`/`hovCloudMinus/Plus`/
    `draggingCloud` grew `[18]`→`[23]`.
  - Both Debug and Release builds clean on first attempt after full wiring. Not run interactively —
    per `CLAUDE.md`, the user verifies. **Next data point needed**: re-benchmark the same surface
    spots with defaults (should already show a real gain from the terrain `kN` fix alone, nothing
    else visually changed), then empirically drop `Light samples` specifically to determine whether
    the transmittance-LUT follow-up is worth scoping.

- **Round 2, same session: `viewSamples` slider testing revealed the atmosphere loop's real
  problem — a fixed sample count over a wildly variable path length, not a single "right" value.**
  User found `viewSamples=4` gives huge FPS gains and looks convincing from the ground looking up,
  but needs progressively more samples at higher altitude/near the terminator, with low values
  producing rainbow banding near the terminator and the atmosphere vanishing entirely past MEO.
  User's hypothesis was floating-point precision breaking down at Earth-scale distances — **ruled
  out**: precision loss doesn't recover just by raising the sample count, but this problem does,
  which is the signature of undersampling, not precision. Root cause: `segLen = tEnd/N_VIEW` uses
  a FIXED sample count over `tEnd`, and `tEnd` (distance to the atmosphere exit) varies enormously
  with viewing geometry alone — ~100km straight up from the ground vs. 2000+km for a near-horizon
  ray (the same geometry as looking toward a low sun near the terminator), and similarly long for
  most rays hitting the shell at a grazing angle from orbit. A fixed low `N_VIEW` badly
  under-resolves the ~8km Rayleigh scale height exactly on those long/grazing rays.
  - **Fix: `N_VIEW` is now adaptive per-ray**, not a single value. `viewSamplesMin` (default 4, the
    user's own validated "looks convincing" ground floor) and `viewSamplesMax` (default 124, the
    prior universal fixed value — already proven correct at all altitudes before this round) are
    both UBO-tunable; the shader derives a target step length from `kAtmosRefTEnd / viewSamplesMin`
    (`kAtmosRefTEnd = 100000.0` = `R_ATMOS - R_EARTH`, the ground-level straight-up reference path)
    and scales `N_VIEW` per-ray to hold that same step length for whatever `tEnd` that specific ray
    actually has, clamped to `[viewSamplesMin, viewSamplesMax]`. Mirrors the terrain march's
    altitude-scaled step count and the cloud march's step-length cap — same "target physical
    resolution, adapt sample count to match" pattern used twice already this session.
    `sat_sky.frag`'s `N_VIEW = int(max(8.0, cloud.viewSamples))` single-value line replaced with
    this derivation; `CloudParams`' `viewSamples` field split into `viewSamplesMin`/`viewSamplesMax`
    (struct size unchanged, 224 bytes — reused the existing `pad4` slot). "View samples" slider
    split into "View samples (min)"/"View samples (max)"; `cloudBufs`/hover/dragging arrays grew
    `[23]`→`[24]`.
  - `lightSamples` (`N_LIGHT`, the `optDepth` sub-march) was NOT made adaptive — the user only
    reported `viewSamples` as impactful; leave as a plain tunable unless similar artifacts show up.
  - Both builds clean. Not run interactively — user verifies: should now get the ground-level FPS
    win from a low `viewSamplesMin` AND correct terminator/MEO visuals simultaneously, without
    manually cranking the slider per viewing situation.

- **Round 3, same session: user confirmed the adaptive `N_VIEW` optimization works well** —
  `viewSamplesMin=4` had visible artifacts, `6` was clean (new default); terrain FPS reached
  40-50fps with other tuning. Remaining complaint: camera angle is now the single biggest FPS
  factor — zenith/ground-facing views >80fps, horizon-facing views ~50fps. Expected (horizon rays
  have both the longest atmosphere path, now correctly handled by the round-2 adaptive `N_VIEW`,
  and the most visible ocean/terrain), but investigated for further low-angle-specific waste.
  - **Found a real "compute expensive detail, then discard it" pattern in ocean's `getNormal`**
    (`sat_sky.frag` ~1449, wave normal from `seaMapDetail` central differences): it always paid
    the full cost of 3 `seaMapDetail` calls (5 octaves each = 15 octave evaluations) whenever
    `altFade>0.01`, THEN blended the result back toward flat (`surfUp`) via
    `mix(waveN, surfUp, max(distFade, 1-altFade))` for anything far away or high-altitude — i.e.
    it always computed the detail even when the blend was about to throw nearly all of it away.
    Horizon views are exactly the case with the most far-away, blended-to-flat ocean on screen
    (`heightMapTracing` just above already correctly gates its own expense by `dist<5000`; this
    one didn't have the equivalent gate). **Fix:** compute `blend` first (cheap — only needs
    `dist`/`altFade`, both already known), skip the three `seaMapDetail` calls entirely when
    `blend >= 0.99` (result would be ≥99% flat anyway). Bitwise-identical output below that
    threshold; the skipped detail above it was already imperceptible.
  - The terrain march's cost for horizon rays is more fundamentally tied to the search itself
    (no equivalent "computed then discarded" pattern found) — flagged as a harder, lower-priority
    follow-up if the ocean fix doesn't close enough of the gap.
  - Both builds clean.

### 2026-07-12 (session 22)
- **C15 complete:** Airglow implemented as three altitude-banded emissive terms in `sat_sky.frag`.
  - **Altitude-range architecture mismatch found before finishing:** the design doc's directive to
    have all three bands "ride the existing N_VIEW atmosphere loop" only holds for green (peak
    96km) and sodium (peak 90km) — both fall inside the loop's own altitude ceiling, since
    `tEnd = min(tAtmos.y, tSurface)` where `tAtmos = raySphere(obsPos, dir, R_ATMOS)` and
    `R_ATMOS = R_EARTH + 100000`, i.e. no sample in that loop ever exceeds ~100km altitude. Red
    (peak 275km, half-width up to 100km) needs samples out to ~350-500km — genuinely unreachable
    from that loop's sample set. Extending the primary loop's far bound to cover it would have
    kept the same 124-step budget spread over a ~5x longer march, coarsening the near-surface
    Rayleigh/Mie resolution that the existing sky color already depends on (most of that signal is
    packed into the first ~50km via the exponential scale-height falloff) — not an acceptable
    trade for one new secondary band.
  - **Resolution:** green + sodium accumulate directly inside the existing N_VIEW loop (added to
    the same per-sample block that already computes `spECEF`/`spUV` for city upwelling — reuses
    those geographic coordinates instead of recomputing them). Red gets its own small supplemental
    march (16 steps) that starts where the primary loop's far bound already ends (`tAtmos.y`) and
    extends out to a `R_EARTH + 500000` sphere exit, only when the sky is open above (`tSurface <=
    0.0` — no ground/terrain blocking the view). This mirrors the C13 lesson (cirrus also needed a
    genuinely separate march once the "ride an existing pass" assumption broke down on contact with
    the actual code) — noting it again here since it recurred on the very next Phase E step.
  - **Night gating:** per-sample geographic day/night dot product (`dot(sampleDirECEF,
    sunDirECEF)`, same `±0.15` smoothstep window `cloudMarch`'s `sampleDayness` uses), not the
    observer's sun elevation — physically correct since the glow originates at the sample's own
    geographic position along the view ray, which can differ from the observer's local time of day
    for grazing/limb rays.
  - **Horizontal patchiness:** reused the existing analytic `warpPerlin3` noise (the same evaluator
    `cloudWarpOffset`/`cirrusDomainWarp` use) rather than a `noiseTex` lookup as the original C15
    design note specified — that note pre-dates the cloud domain warp's migration from a texture
    read to this analytic evaluator (see `cloudWarpOffset`'s own comment on why), so this follows
    the current code's established pattern instead of the stale plan text. No new texture, no new
    binding either way.
  - **Physics constants hardcoded, brightness exposed:** peak altitude/half-width/color per band
    are compile-time constants (real, near-fixed physical values, not scene-dependent) rather than
    `CloudParams` fields — only per-band gain (`airglowGain` master + `airglowGreenGain`/
    `airglowRedGain`/`airglowSodiumGain`) is UBO-exposed, since brightness is a genuine first-pass
    visual guess that needs tuning after seeing it render, unlike the altitudes.
  - **`CloudParams` UBO grew 176→192 bytes:** all three previous pad slots (`shadowSteps`/
    `cirrusWindAngle`/`cirrusStretch`) were already consumed by C13, so the 4 new airglow gain
    floats needed a real size increase, not a repurposed pad — kept the global section a multiple
    of 16 bytes (12→16 floats) so the `layers[4]` array's std140 alignment doesn't shift. Both the
    GLSL `CloudParams` block and the C++ `GpuCloudParams` struct (`SatelliteSim.h`) were updated in
    lockstep; `static_assert(sizeof(GpuCloudParams) == 192)` catches future drift.
  - New settings-window sliders: "Airglow gain" (master, 0-5), "Airglow green"/"Airglow red"/
    "Airglow sodium" (per-band, 0-3 each). `hovCloudMinus`/`hovCloudPlus`/`draggingCloud` arrays and
    `cloudBufs` bumped `[13]`→`[17]`. Persisted in `settings.json` under `clouds.airglow_*`.
  - One build-time snag: GLSL reserves `patch` as a tessellation-shader keyword — a local variable
    named `patch` (for the domain-warp brightness multiplier) failed to compile with a cryptic
    "syntax error, unexpected PATCH" at the point of first use; renamed to `airPatch`.
  - Build clean (`cmake --build build`). Not run — per CLAUDE.md, the user tests interactively;
    brightness constants (`kAirglowScale` and the three default gains) are first-pass guesses and
    will likely need visual tuning via the new sliders.
  - **Defaults baked in from the user's tuned `settings.json`** after visual approval, same session:
    all `clouds.*` (coverage/density/altitudes/drift/sun+ambient gain/HG g/march+light+shadow
    steps/airglow gains) and `photometry.*` (brightness/day suppression/mirror boost/vis
    threshold/highlight flare) members in `SatelliteSim.h` now default to those values instead of
    the original placeholder guesses. Camera/observer/audio/UI-scale settings were deliberately
    left as-is (session state, not tuning) per explicit user scoping.

- **Raymarch-from-inside-a-volume bug found and fixed** (reported by the user after visually
  approving C15): "odd banding patterns, placing red glow above the observer" plus "seams" at the
  horizon, worst with the new red airglow band but present in cloud/cirrus rendering too. Two
  distinct root causes, both in `sat_sky.frag`:
  1. **`raySphere` numerical fragility (shared by all 29 call sites).** `c = dot(ro,ro) - r*r`
     subtracts two ~1e13-magnitude float32 numbers (`ro`/`r` are both ~R_EARTH≈6.37e6 m) — precision
     collapses catastrophically exactly at grazing/near-tangent rays, i.e. every horizon, in every
     shell march that calls it (clouds, cirrus, airglow, atmosphere, ocean, terrain). Fixed by
     reformulating `c` as `(|ro|-r)*(|ro|+r)` — a direct small-number subtraction instead of a
     squared-magnitude cancellation — the standard fix for planetary-scale ray-sphere tests in
     single precision. Does not fully eliminate float32's precision floor at exact tangency (`b*b`
     is still large there), but removes the much larger, unconditional cancellation error that was
     present at every distance, not just exact tangency.
  2. **Missing above/inside/below handling in the new red-band march (a real bug introduced this
     session, not pre-existing).** The march unconditionally started at `tAtmos.y` (the 100km
     sphere's forward exit), assuming the observer is always below that boundary. The "Raise
     Elevation" (Q) control has no altitude cap and its climb rate scales with current height, so
     it's easy to fly up INTO the 100-500km band to inspect the new layer up close — exactly what
     motivated this bug report. Once `obsEffH` exceeds ~100km and the view ray points outward/away
     from Earth, `raySphere`'s forward root on the inner sphere goes negative (that sphere is now
     behind the camera), so the march began marching from behind the camera, sweeping back through
     the observer's own position — reproducing "red glow above the observer" exactly (zenith is the
     outward-ray case). Fixed by giving the red march the same below/inside/above shell-relative
     classification `cloudMarch`/`cirrusMarch` already use, keyed on `obsEffH` against the band's
     [100km, 500km] bounds, instead of assuming a fixed relationship to the observer.
  - Also added a defensive `max(0.0, ...)` clamp on the primary `N_VIEW` loop's `tEnd` (same
    negative-forward-root scenario applies to the base atmosphere march itself when the observer is
    above `R_ATMOS` looking outward — not previously reachable in normal play, but now reachable via
    the same elevation-climb path that surfaced the red-band bug).
  - Build clean (`cmake --build build`). Not run interactively — per CLAUDE.md, the user verifies.

- **Follow-up: cloud banding "viewed from the side" — third, distinct root cause found in
  `cloudMarch`'s stepping, not `raySphere`.** The two fixes above resolved airglow but the user
  reported cloud banding was still "significant for all clouds viewed from the side" — i.e. common
  low-to-moderate viewing elevations, not just the extreme near-horizon grazing case `raySphere`'s
  precision fix targets. Root cause: the C8 "altitude-stratified stepping" formula
  (`stepLen = (shellThick/N) / max(abs(dir.z), 0.02)`) keeps VERTICAL resolution constant across
  ray angles by inflating the REAL 3D step length for oblique rays — uncapped, up to 50× the
  vertical step at the `0.02` floor. Cloud noise has comparable structure scale horizontally and
  vertically, so a ray looking at a cloud tower from the side takes real 3D steps up to 50× coarser
  than a straight-up ray, badly undersampling the noise field along that ray — this is what more
  march steps "fixes," because raising `cloud.marchSteps` shrinks the vertical step (and therefore
  the inflated oblique step too) for *every* ray, including already-fine vertical ones, which is
  why it was expensive.
  - **Fix:** cap the real step length at a **fixed absolute distance** (`kCloudMaxStepM = 250.0`),
    not a multiple of `shellThick/N`. Steep/vertical rays are unaffected — their natural step is
    already well under 250m at any reasonable `marchSteps` setting. Oblique/side-view rays trade
    extra loop iterations for a bounded real step size instead of an ever-widening one. The cap is
    deliberately a fixed meters value rather than e.g. `4×vertStep`: a multiple-based cap would
    shrink in lockstep with `vertStep` at high `marchSteps` slider settings, and the worst-case
    grazing-ray iteration count (~80km cap / step size) would grow unboundedly with the slider
    instead of staying bounded — risking silently exceeding the loop's 512-iteration hard cap
    (truncating the march) at the slider's upper range. The fixed-meters cap keeps worst-case
    iterations bounded at ~80000/250=320, safely under 512, regardless of the `marchSteps` slider.
  - This only affects `cloudMarch` (the reported bug). `cirrusMarch` uses plain uniform real-
    distance stepping (`segLen = (tExit-tEnter)/N_CIRRUS`, no altitude-stratification), so it
    doesn't share this specific failure mode — not touched.
  - Build clean (`cmake --build build`). Not run interactively — per CLAUDE.md, the user verifies.

- **Follow-up: "Light steps" slider found to be completely dead — wired up.** User reported that
  even at `march steps=4, light steps=1` the sim held at 20fps, while quality stayed surprisingly
  good — suspicious, since that combination should be cheap if those sliders actually controlled
  cost. Root cause: `cloud.lightSteps` (the `CloudParams` UBO field the "Light steps" slider writes)
  was declared but **never read anywhere in `sat_sky.frag`** — the sun self-shadow cone inside
  `cloudMarch` hardcoded `const int N_CONE = 6` regardless of it. This cone runs on every outer
  march sample that lands inside a cloud (not gated by `cloud.marchSteps` either), each doing up to
  6 more `cloudDensity()` calls (2-3 3D texture fetches apiece) plus an `earthCloudsTex` fetch —
  this dead slider was very likely the dominant, previously-uncontrollable cost, and explains both
  symptoms: quality stayed good because the real shadow-quality knob was silently stuck at 6, and
  performance stayed bad for the same reason. Fixed: `N_CONE = max(1, int(cloud.lightSteps))`, so
  the existing slider (range 1-16) now genuinely trades shadow quality for FPS.
  - **Related, not fixed this session — flagged for the user to decide:** the outer march step
    count also has a floor independent of the slider: `groundSteps = max(48.0, cloud.marchSteps)`
    (ground-level branch, i.e. `obsEffH < kMarchStepsAltStart = 2000m`) means `marchSteps` values
    below 48 have **zero effect** at ground level — the slider's 4-47 range is currently dead too.
    This is a pre-existing, seemingly deliberate floor (comment: "matches the old unconditional
    minimum"), not something introduced this session, so left as-is pending user input on whether
    to lower/remove it.
  - Build clean (`cmake --build build`). Not run interactively — per CLAUDE.md, the user verifies.

- **Follow-up: distance-gated cloud self-shadow cone + extended render distance.** With `lightSteps`
  now live, the user asked to go further: restrict the shadow cone (the dominant per-sample cost) to
  *close* clouds only, and spend the freed budget extending `cloudMarch`'s render-distance cap to
  reduce visible pop-in near the horizon/stratosphere.
  - `sampleDist = t + step*0.5` (the real distance from the camera to the current march sample,
    same value already used to build `p`) drives `shadowFade = 1 - smoothstep(shadowMaxDistM*0.6,
    shadowMaxDistM, sampleDist)`. The `N_CONE` sub-march is skipped entirely once `shadowFade` drops
    near 0 (default `shadowMaxDistM = 15000m`), and its result is scaled by `shadowFade` inside the
    fade band `[0.6, 1.0]×shadowMaxDistM` so there's no hard lighting seam at the cutoff — distant
    clouds still get *some* self-shadow going into the transition, easing toward flat/fully-lit
    rather than snapping.
  - Nice side-effect for the user's other open concern ("LEO is still tough"): `sampleDist` is
    camera-relative, not observer-altitude-relative, so an orbital camera (400km+ from any surface
    cloud) puts effectively every sample beyond `shadowMaxDistM` automatically — the shadow cone is
    now essentially free from orbit too, not just for distant ground-level clouds.
  - `tExit = min(tExit, tEnter + 80000.0)` (hardcoded 80km) replaced with `cloud.maxRenderDistM`
    (new UBO field, default 150000m = 150km — was too conservative once the 250m step cap and the
    shadow-cone fade freed up per-far-sample cost; the old value was the direct cause of the
    reported pop-in).
  - The march's hard iteration cap (`for (int i=0; i<512 && t<tExit; ++i)`) was **also** a fixed
    512, sized for the old 80km cap at the 250m step-length floor (worst case 80000/250=320,
    comfortably under 512). Raising `maxRenderDistM` to 150km pushes the worst case to 600 —
    **would have exceeded 512 and silently truncated the march**, undoing the render-distance
    extension for exactly the grazing rays that need it. Replaced with a cap derived from
    `cloud.maxRenderDistM/kCloudMaxStepM + 32`, clamped to a 2048 ceiling so an extreme slider value
    can't create a pathological loop.
  - Two new `CloudParams` UBO fields (`shadowMaxDistM`, `maxRenderDistM`) + 2 explicit pad reserves
    to keep the global section's std140 alignment a multiple of 16 bytes (176→192 was already tight
    from C15; adding exactly 2 meaningful fields needed 2 more to stay 16-byte-aligned before the
    `layers[4]` array) — struct grew 192→208 bytes, `static_assert` updated in lockstep. New sliders:
    "Shadow max dist (m)" (1000-60000, default 15000), "Render dist (m)" (20000-400000, default
    150000). `cloudBufs`/`hovCloudMinus`/`hovCloudPlus`/`draggingCloud` bumped `[17]`→`[19]`.
    Persisted under `clouds.shadow_max_dist_m`/`clouds.max_render_dist_m`.
  - Build clean (`cmake --build build`). Not run interactively — per CLAUDE.md, the user verifies.

### 2026-07-12 (session 21)
- **C13 complete:** Cirrus promoted from a flat 2D decal to a genuine volumetric shell march.
  - **Architecture mismatch found before writing code:** the session-20 kickoff prompt assumed
    cirrus (`layers[1]`, ~11km) was a separate flat paste sitting next to an independent low-cloud
    volumetric shell, and suggested "wire a second `cloudMarch` call reusing the ~11km shell
    height." Reality (from commits since session 20, not yet logged here): `cloudMarch` already
    treats `layers[0].shellAltM` (2km) as its base and `layers[1].shellAltM` (11km) as its TOP —
    i.e. the entire 2-11km span is ONE merged low/mid volumetric shell, with per-column tower
    height (`colH`) determining how many columns actually reach 11km. There is no independent
    volumetric cirrus band to extend — the flat `evalCloudLayer` paste at `layers[1]` was the
    *only* cirrus-specific representation, and it only renders far/high (crossfaded out near the
    ground via `kCloud3DFadeStart/End`), so ground-level cirrus effectively didn't exist as its
    own phenomenon before this session.
  - **Approach taken:** new standalone `cirrusMarch()` function (`sat_sky.frag`, right before
    `main()`), decoupled from `cloudMarch` entirely. Own thin shell (`kCirrusThicknessM = 700m`
    centered on `layers[1].shellAltM`), own short march (`N_CIRRUS = 14` — thin shells need far
    fewer samples), own low extinction coefficient (8e-5 vs. the low-cloud deck's 3e-3 — reads as
    translucent even across a full traversal). Crossfades against the same `evalCloudLayer`
    layer[1] flat paste using the identical `kCloud3DFadeStart/End` band cloudMarch/layer0 use.
  - **Anisotropic stretch — one dead end, one working approach:** first attempt was a per-sample
    tangent-plane decomposition (build local East/North tangent basis at each sample point, project
    the noise argument onto it, scale, recombine). This is mathematically a no-op: the argument fed
    to `cloudNoiseTex` is `direction * frequency`, which is purely radial from the sample point's
    own tangent frame's perspective, so it has zero projection onto that frame's tangent axes by
    construction. Fix: use a single FIXED global wind axis (`cloud.cirrusWindAngle`, equatorial-
    plane azimuth) and compress the sampling direction's component along that fixed axis
    (`dirStretched = dir + windAxis * dot(dir, windAxis) * (1/stretch - 1)`) before the frequency
    multiply — this is well-defined globally (not degenerate per-sample) and genuinely compresses
    noise-space distance per geographic degree traveled along the wind axis, producing elongated
    features. Reuses the existing isotropic `cloudNoiseTex` (binding 8) — no new bake, no new
    binding, per the "cheap path first" plan; fallback (b), a dedicated elongated-Worley bake, was
    not needed.
  - **Lighting kept deliberately simple:** sun-only, matching `evalCloudLayer`'s existing formula
    exactly (`max(0,cloudSunDot+0.1)*sunGain*dayFrac`), rather than porting `cloudMarch`'s full
    Beer-Powder/multi-scatter/sky-ambient/city-upwelling model. Reasoning: a thin high deck is
    dominated by direct/scattered sunlight, and matching the flat paste's exact lighting formula
    means the two renders color-match at the crossfade boundary instead of visibly shifting tone.
  - **New `CloudParams` fields:** repurposed the two unused `pad1`/`pad2` floats (both C++
    `GpuCloudParams` and the GLSL `CloudParams` UBO) as `cirrusWindAngle` (radians) and
    `cirrusStretch` (elongation factor) — no struct size change, no descriptor rebuild needed.
    New CPU members `cloudCirrusWindDeg` (default 40°) / `cloudCirrusStretch` (default 4.0),
    two new "Clouds" section sliders ("Cirrus wind (deg)", "Cirrus stretch"), persisted in
    `settings.json` under `clouds.cirrus_wind_deg`/`clouds.cirrus_stretch`. `hovCloudMinus/Plus`/
    `draggingCloud` arrays and `cloudBufs` bumped from `[11]` to `[13]` to match.
  - Build clean (`cmake --build build`): shader compiled, C++ compiled, link succeeded. Not run —
    per CLAUDE.md, the user tests interactively.
  - **Note for next session:** the session-20 log below is stale relative to the actual shader —
    several commits ("Kind of fixed shadows", "Update", "Nice clouds!", "Night glow effects",
    "Fixed issue with sun flare") landed real cloud-shadow/lighting/domain-warp work between
    session 20 and this one without corresponding log entries. Don't trust this file's session log
    as a complete history of `sat_sky.frag` — read the current code for ground truth on anything
    architecture-sensitive before planning C14-C16.
- **C13 follow-up fixes (same session, after user feedback on the first pass):**
  - **Curved streaks:** the original fixed global `cloud.cirrusWindAngle` stretched every streak
    on the dome in the same literal direction, reading as repetitive/uniform. Fixed by perturbing
    the wind angle itself per-sample with a new low-frequency curl-like noise, `cirrusWindAngleAt`
    (reuses the existing `warpPerlin3` analytic noise from `cloudWarpOffset`, own frequency/drift
    tuned for jet-stream-scale curl rather than cloud-shape scale) — `wa = cloud.cirrusWindAngle +
    cirrusWindAngleAt(dirECEFDrift)`, recomputed every march sample so the stretch axis bends
    smoothly across the sky. Also added `cirrusDomainWarp` (a second, lower-frequency warp octave
    layered on `cloudWarpOffset`, cirrus-only) since cirrus samples the same 128-voxel
    `cloudNoiseTex` at a coarser frequency than cumulus, leaving proportionally less of the baked
    volume's own period to hide the repeat behind — the single warp octave tuned for cumulus
    wasn't enough at cirrus's scale. Both reuse existing noise infra; no new texture/binding.
  - **Depth-order bug (cirrus rendering in front of nearer cumulus):** found the same bug in TWO
    places. (1) `cirrusMarch`/`cloudMarch` are plain sequential alpha-composites into `color` with
    no true depth test — whichever runs LAST always draws on top regardless of actual distance.
    Cirrus (~11km) is always farther than the merged low/mid deck (2-11km) for a ground-level
    observer (concentric-shell exit distance grows monotonically with shell radius for an
    observer inside both), so `cirrusMarch` must run BEFORE `cloudMarch`, not after — swapped the
    call order in `main()`. (2) The flat `evalCloudLayer` loop had the identical bug: it iterated
    `li = 0..3` ascending, drawing layer0 (low/near) before layer1 (cirrus/far) — backwards for
    the same reason. Fixed by iterating `li = 3..0` descending (layers are conventionally ordered
    low-to-high altitude by index, so high-to-low index draws far-to-near). Neither fix changes
    per-layer math, only composite order — low risk.
  - Build clean (`cmake --build build`) after both fixes. Not run — per CLAUDE.md, user tests
    interactively.
- **Unrelated satellite-flare regression found and fixed the same session (build hygiene +
  a real latent shader bug, NOT a cirrus-code bug):**
  - User reported the peak specular of some satellite flares (most visible on the SpaceX AI
    datacenter constellation's Phong disk, viewed from orbit near the anti-solar point where the
    whole disk should light up) rendering as pure black — a "hole" punched in the brightest part
    of the flare. Reported as starting "around the cirrus change," but persisted even after
    reverting 2 commits of source and after resetting/rescaling the day-suppression and
    mirror-boost settings — ruling out both a cirrus-code regression and a settings/tuning issue.
  - **Found a genuinely stale build artifact:** `build/shaders/sat_flare_smooth.comp.spv` (and its
    `build/Debug` copy) had no corresponding source file anywhere (`shaders/sat_flare_smooth.comp`
    doesn't exist, no git history for that filename, unreferenced in `src/`) — leftover from some
    earlier abandoned experiment, never cleaned up because the CMake shader pipeline's
    `copy_directory` step only ever adds/overwrites, never prunes outputs whose source is gone.
  - Deleted `build/shaders`, `build/Debug/shaders`, `build/Release/shaders` and did a full rebuild,
    forcing every shader (`sat_flare.comp`, `sat_orbit.comp`, `sat_point.vert/frag` included) to
    recompile from current source — confirmed this was NOT simply stale-vs-source drift for the
    files that matter (CMake's `DEPENDS`-based incremental compile was already correctly picking up
    real source changes), but doing the clean rebuild is what actually resolved the visual bug,
    pointing to some kind of incremental-build/link inconsistency rather than pure staleness.
  - **Also fixed a real latent bug while investigating** (`shaders/sat_flare.comp`, mirror-peak +
    Phong specular terms): `dot(refl0, satToObs)` and the secondary-surface equivalent were only
    lower-clamped (`max(0.0, …)`), never upper-clamped to 1.0. These dot products are between
    vectors that are supposed to be unit length, so they should be mathematically bounded to
    `[-1,1]` — but float rounding can push them a hair above 1.0. Invisible at low `specExp0`, but
    `mirrorExp = max(specExp0 * mirrorBoost, 8000)` can reach the tens of thousands (up to 200,000
    for the Reflect Mirror type at max slider), where even a ~0.01% overshoot in the `pow()` base
    blows up to `+Infinity` in float32 — propagating through `spec0`/`flare`/`effectFlare` and
    rendering as a black hole exactly at the flare's peak. This is also why no amount of scaling
    `brightnessScale`/`mirrorBoost` could fix it: any finite multiplier of `Infinity` is still
    `Infinity`. Changed both `spec0`'s and `mirrorPeak`'s dot products to `clamp(dot(...), 0.0,
    1.0)` — since `pow(x, N)` for `x` clamped to `[0,1]` can never exceed 1.0 regardless of how
    large `N` is, this is a complete, provably-bounded fix for this overflow class, kept
    permanently regardless of whether it was the actual trigger this time.
  - **Lesson for future sessions:** if a rendering regression persists across source reverts AND
    setting changes, suspect the build directory before spending more time on shader-source
    theories — this project's `file(GLOB CONFIGURE_DEPENDS ...)` + per-file `add_custom_command`
    shader pipeline is generally sound for content changes, but doesn't prune orphaned outputs, and
    at least one incremental-build inconsistency was observed that a full shader-output wipe fixed
    outright. Cleaning is cheap (just `build/*/shaders`, not the whole `build/` tree — that would
    force a slow FetchContent re-fetch of glfw/glm/miniaudio) and should be an early diagnostic
    step, not a last resort.

### 2026-07-12 (session 20)
- Planning-only session, no code changes. Discussed four next volumetric layers (from a prior
  session's rough estimate: Cirrus rework, Anvil, Airglow, Aurora) and locked design decisions for
  each, added as Phase E (C13-C16) above:
  - **Cirrus:** volumetric rework of the C4 flat overlay; needs a wispier/stretched noise (anisotropic
    domain warp on existing `cloudNoiseTex` first, dedicated bake as fallback); fixed altitude shell.
  - **Anvil:** threshold-gated lateral spread near `colH` in the existing height-profile code;
    new `anvilThreshold`/`anvilSpread` sliders in `CloudParams`.
  - **Airglow:** three emissive altitude bands (green/red/sodium) riding the existing `N_VIEW` loop;
    researched real airglow altitudes/widths/colors as defaults (see C15); time-domain domain-warped
    banding to avoid repetition.
  - **Aurora:** most novel — new emissive-only curtain-shell march centered on the geomagnetic pole
    (80.7°N/72.7°W north, 80.7°S/107.3°E south), storm-strength slider, generalized curtain primitive
    for future aurora types, day-suppression + Earth-occlusion requirements locked.
  Phase E sequenced ahead of finishing C9/C11/C12 by user decision. Kickoff prompt for C13 added at
  the top of this file under "Immediate Next Step."

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

### 2026-06-28 (session 15)
- **C5 complete:** `VulkanContext::createImage` gains optional `depth = 1` parameter.
  When `depth > 1`: `imageType = VK_IMAGE_TYPE_3D`, `extent = {w, h, depth}`.
  All existing 2D callers unaffected (default argument). `VK_IMAGE_USAGE_STORAGE_BIT`
  was already passable via `usage` — no further changes needed. Build clean.

### 2026-06-28 (session 16)
- **C6 complete:** `shaders/cloud_noise.comp` bakes a 128³ RGBA8 Perlin-Worley + 3× Worley
  erosion volume at init. `createCloudNoisePipeline()` in `SatelliteSim.cpp`:
  creates the 3D image (STORAGE_BIT|SAMPLED_BIT, depth=128 → VK_IMAGE_TYPE_3D), dispatches
  the bake in a one-time command (16×16×16 groups × 8³ local = 128³ threads), transitions
  GENERAL→SHADER_READ_ONLY_OPTIMAL, then destroys the temporary pipeline+desc set.
  Sky desc set expanded 9→10 bindings: binding 8 = COMBINED_IMAGE_SAMPLER pointing at
  cloudNoiseView; pool samplers 7→8; `writes[10]` with writes[8]=cloudNoise, writes[9]=UBO.
  `sat_sky.frag` declares `layout(binding=8) uniform sampler3D cloudNoiseTex`; temp debug
  adds 6% Perlin-Worley tint to sky output to confirm the volume is live and tiling.
  `cloudNoise{Img,Mem,View,Sampler}` members added to header; cleanup wired in `cleanup()`.
  Build clean.

### 2026-06-28 (session 17)
- **C7 complete:** Volumetric cloud shell march implemented in `sat_sky.frag` (shader-only).
  Three new functions added before `main()`:
  - `remap(v, lo, hi, newLo, newHi)` — clamped linear remap utility
  - `cloudDensity(noiseUVW, coverage, density, heightProfile)` — Nubis remap + 3-octave Worley erosion
  - `cloudMarch(obsPos, dir, tSurface, enuX, enuY, enuZ, sunDir, sunDirECEF, obsEffH, inout color)` —
    bounded shell march with adaptive big/fine stepping and empty-space skip
  Shell bounds: `raySphere` against `R_EARTH + layers[0].shellAltM` and `R_EARTH + layers[1].shellAltM`.
  Observer cases: below (enter=shellB.x, exit=shellT.x), inside (tEnter=0.001, exit=min(shellT.y, shellB.x)),
  above (early return; cloudAltFade=0 anyway). Terrain clamp: `tExit = min(tExit, tSurface)`.
  Loop: hard cap 512 iterations; bigStep = stepLen×4 when density < 0.001.
  In-scatter: gray HG via `phaseM(cosA)`, geographic sun dot `dot(normalize(pECEF), sunDirECEF)`.
  Cross-fade: `cloudAltFade = 1 - smoothstep(3000, 8000, obsEffH)` mirrors ocean altFade.
  Call site: after cloud layers loop (line ~933), before auto-exposure. Build clean.

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

### 2026-07-01 (session 19)
- **C8 complete:** Cloud lighting upgraded from placeholder gray to physically-based spectral model.
  - `sunColorCloud` = atmospheric sun color at shell entry (`optDepth` toward sun, same formula as
    main atmosphere loop). Gives orange/red clouds at sunset/sunrise. Night-side clouds gated by
    Earth shadow check (raySphere R_EARTH) → zero direct sun on dark side of planet.
  - `sampleDayness` per march sample from geographic `dot(pECEF, sunDirECEF)` → ambient sky dome
    transitions from `vec3(0.35, 0.55, 0.85)` (blue day dome) to `vec3(0.001)` (near-zero night).
    Cloud tops (hNorm=1) get 2.7× more ambient than bases (hNorm=0).
  - City light upwelling: `textureLod(earthNightTex, pUV, 3.0)` at night in lower half of cloud
    shell (hNorm < 0.5); warm orange tint into overcast bases over city regions.
  - Altitude-stratified march stepping: `stepLen = (shellThick/N) / max(abs(dir.z), 0.02)`.
    Each step advances equal altitude regardless of ray angle → no more oblique-angle slab artifacts
    without increasing step count. Replaces `stepLen = (tExit - tEnter) / N`.
  - Build clean.

### 2026-06-30 (session 18)
- **C7 cloud debugging — root cause identified and architectural fix applied:**
  - Investigated discrete altitude slab artifacts (debug mode 2 showing stacked uniform-color ovals
    instead of smooth 3D blob volumes). Long investigation ruled out: terrain march geometry, raySphere
    precision, LOD step-size, distance-based effects (those would produce observer-concentric rings,
    not Earth-altitude-parallel layers).
  - **Root cause 1 (architectural):** `cloudDensity` used the 3D noise R channel (Perlin-Worley) for
    cloud *presence* with Z driven by altitude (hNorm × kVertTiles). The Perlin R channel has ~3
    positive Z-lobes across 1.5 kVertTiles, producing exactly 3 cloud altitude bands when threshold-gated.
  - **Fix applied:** split `cloudDensity` into two UVW coordinates — `uvwPresence` (Z=posZ, constant
    per column) for the Perlin R threshold, and `uvwDetail` (Z=hNorm×kVertTiles) for Worley erosion
    G/B/A only. Worley cells are spherical and do not create horizontal slabs. Cloud existence is now
    purely horizontal-noise-driven; vertical extent from hFade/colH.
  - **Root cause 2 (performance):** insufficient march steps. At default ≈48 steps, coarse sampling
    creates hard density discontinuities that project as discrete slab layers at all elevation angles.
    **150+ steps resolves the layering from sea level.** The step count is the primary quality lever.
  - Several noise/threshold experiments were attempted and reverted: Z-compressed texture bake in
    cloud_noise.comp (made towers worse), fixed threshold (removed horizontal breaks), coverage
    multiplier. These are documented here as non-solutions. cloud_noise.comp is at original values.

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
