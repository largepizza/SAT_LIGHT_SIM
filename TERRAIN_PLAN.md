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

### Descriptor set plan (sky pipeline, bindings 0-8)
| Binding | Current | Future |
|---------|---------|--------|
| 0 | GlowBuf SSBO | GlowBuf SSBO |
| 1 | noiseTex sampler | noiseTex sampler |
| 2 | moonTex sampler | moonTex sampler |
| 3 | — | earthDay sampler (8K RGBA8) |
| 4 | — | earthNight sampler (8K RGBA8) |
| 5 | — | earthSpecular sampler (8K R8, ocean mask) |
| 6 | — | earthNormal sampler (8K RGBA8 tangent-space) |
| 7 | — | earthClouds sampler (8K R8) |
| 8 | — | earthElevation sampler (8K R16 or R8) |

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
  `earth_elevation.jpg` confirmed: 21600×10800 grayscale 8-bit ETOPO1 1-arcmin DEM.  
  Encoding: elev_m = pixel/255.0 × 19746 − 10898; max(0, elev_m) = above-sea terrain.

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

- [ ] **Step 6** — Ocean wave material  
  In ground branch: sample specular map (white=ocean). Ocean pixels get animated wave
  normals (two FBM scrolling octaves) + Schlick Fresnel + specular highlight.

- [ ] **Step 7** — Night lights → sky glow  
  In sky glow loop: for bins near the horizon, sample earthNight at the geographic
  position of the bin centre and add luminosity as emissive contribution.

### Terrain elevation
- [x] **Step 8** — Elevation raymarching  
  Elevation texture loaded as `VK_FORMAT_R8_UNORM` with full mip chain (binding 5).  
  Two-phase terrain march in `sat_sky.frag`:  
  - Phase 1: 48 uniform steps, march range [0, min(tBase.x, 300km)]  
  - Phase 2: 8-step binary search on first terrain overshoot  
  - Terrain hit shaded same as sea-level (sphere-normal Lambertian + day/night textures)  
  - Sea-level fallback if no terrain hit; flat colour if no sphere intersection.  
  No terrain-slope normals yet (use sphere normal for shading). Done session 2026-06-23.

### Clouds
- [ ] **Step 9** — Volumetric cloud layer  
  Load earthClouds texture. Within atmosphere march, add cloud density evaluation
  (2km–10km shell). HG phase function. Sun rays through clouds from existing `optDepth()`.

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
- Step 2: Confirmed `earth_elevation.jpg` is 21600×10800 8-bit grayscale ETOPO1 1-arcmin DEM
- Step 8: Elevation terrain raymarching implemented in `sat_sky.frag`
  - Elevation texture (R8_UNORM, full mip chain) added as binding 5; sky desc set 5→6 bindings
  - Ground section refactored: 48-step coarse march + 8-step binary search
  - March range: [0, min(tBase.x, 300km)]; tShell.y used when ray is upward (horizon mountains)
  - Terrain hits shade with sphere-normal Lambertian (same as sea-level path)
  - Graceful fallback: if elevation file missing, placeholder sampler used (march finds no hits)
  - Build clean
