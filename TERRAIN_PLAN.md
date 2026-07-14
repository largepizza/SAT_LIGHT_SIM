# Terrain / Earth Rendering Plan

Tracks implementation of Earth texturing, terrain elevation, ocean materials, night lights,
clouds, and orbital camera mode. Read this at the start of any terrain-related session.

---

## Immediate Next Step

**Performance data point needed (session 24 follow-up):** the user needs to re-benchmark ground-
level FPS (Release build, same spots as before) now that `N_VIEW`/`N_LIGHT`/ocean quality are
UBO-tunable sliders, and specifically test dropping `Light samples`. If that alone recovers a lot
of FPS, the next task is a real transmittance LUT (2D texture, altitude × sun-angle, replacing
`optDepth`'s inner march at its 4 call sites in `sat_sky.frag`) — see session 24 log for why this
wasn't just built speculatively. Not yet started.

**C16 — Aurora (geomagnetic curtain primitive)** remains the next content feature once perf work
settles. See Phase E below for the full spec. C15 (airglow) is complete — see session log entry
before starting; note the actual implementation deviated from the original "ride the N_VIEW loop
for all three bands" plan (red needed its own supplemental march — see log for why) and C16 should
expect a similar need to re-derive the concrete approach from the current shader rather than the
original C13-era plan text. Also note `N_VIEW` is no longer a compile-time constant (session 24) —
any new code referencing it must read `cloud.viewSamples` via the same locally-computed-`int`
pattern the existing loop now uses, not assume a fixed 124.

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
- [ ] **C12 — Reflect-Orbital sky beams.** New `ReflectBeams` SSBO (sat ENU pos + ground target +
  intensity), written by the compute pass (mirror target/normal already at `sat_orbit.comp:272-313`) or
  CPU, capped to N nearest active beams. In the cloud/fog march, accumulate beam in-scatter
  `phase(view·beam) × proximity(dist to beam line) × mediumDensity(p) × shadow(cloud/terrain)`.
  *Done when:* visible shafts from sunlit Reflect sats to night-side targets, occluded by cloud/terrain.

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

- [ ] **C16 — Aurora (geomagnetic curtain primitive).** Most novel step — new geometry, likely a new
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
