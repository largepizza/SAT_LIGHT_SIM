# Ocean Shader — New Context Handoff Prompt

Copy this prompt verbatim into a new Claude Code session.

---

## Context

This is `c:\Code\Shader_Fun` — a Vulkan satellite simulation. The primary sim is `SatelliteSim`.
The renderer is a fullscreen raymarched fragment shader `shaders/sat_sky.frag`.
Read `CLAUDE.md` and `TERRAIN_PLAN.md` before touching anything.

We just finished a basic ocean wave shader (Step 6 in TERRAIN_PLAN.md) and identified four problems.
Your job is to redesign and reimplement the ocean material properly.

---

## Architecture you need to know

### Shader: `shaders/sat_sky.frag`

The ground-rendering path (Phase 3, ~line 538) runs when `tSurface > 0`:
- `tHit > 0` → terrain march found a hit; uses `hitUV`, `hitPt`, `terrainNorm`
- `tHit < 0 && tSeaLvl > 0` → sea-level sphere hit; computes `hitPt`, `uvSurf` from lat/lon
- Ocean wave code runs only in the `tHit < 0` branch

The terrain march (Phase 1, ~line 306) checks `rayH < terrainH` where
`terrainH = max(0.0, texture(earthElevTex, uv).r * 8848.0)`. Ocean elevation is 0, so the march
never hits ocean — the sea-level sphere fallback always handles it.

### Push constants (`SatDrawPC`, 128 bytes, `shaders/sat_sky.frag` layout):
```glsl
layout(push_constant) uniform PC {
    mat4  skyView;      // offset 0
    float fovYRad;      // offset 64
    float aspect;       // offset 68
    float gmst;         // offset 72 — Greenwich Mean Sidereal Time (radians, ~2PI/day)
    float pad;          // offset 76 — UNUSED, available for waveTime
    vec4  sunDirENU;    // offset 80  — xyz=sun dir in ENU, w=sin(sun elevation)
    vec4  moonDirENU;   // offset 96
    vec4  obsECEFDir;   // offset 112 — xyz=observer ECEF unit vec, w=altitude above sea (m)
} pc;
```

`pad` at offset 76 is unused. Repurpose it as `waveTime` (float, seconds from
`glfwGetTime()` or equivalent wall-clock). Fill it in `SatelliteSim::recordDraw()`.
The C++ struct is `SatDrawPC` in `src/simulations/SatelliteSim.h` — find the
`float pad[2]` array and rename `pad[0]` to `waveTime`.

### Descriptor bindings (sky pipeline):
| Binding | Name | Format | Content |
|---------|------|--------|---------|
| 0 | GlowBuf | SSBO | satellite sky glow histogram |
| 1 | noiseTex | sampler2D | 128×128 tiling RGBA noise, REPEAT wrap |
| 2 | moonTex | sampler2D | moon surface |
| 3 | earthDayTex | sampler2D | 8K SRGB day texture |
| 4 | earthNightTex | sampler2D | 8K SRGB night/city-lights |
| 5 | earthElevTex | sampler2D | 21600×10800 R8_UNORM elevation (p→p×8848 m, ocean=0) |
| 6 | earthSpecTex | sampler2D | 8K R8_UNORM ocean mask (white=ocean, black=land) |

### Atmosphere functions already in `sat_sky.frag`:
```glsl
float phaseR(float cosA)           // Rayleigh phase
float phaseM(float cosA)           // Mie phase
vec2  optDepth(vec3 p, vec3 d)     // optical depth along ray
vec2  raySphere(vec3 o, vec3 d, float r)  // ray-sphere intersection
```
`BETA_R`, `BETA_M`, `H_R`, `H_M`, `R_EARTH`, `R_ATMOS`, `N_VIEW` are all in scope.

### ENU coordinate system (in-shader):
- `obsPos = vec3(0, 0, R_EARTH + altitude)` — observer at ENU origin
- `dir` — unit view ray in ENU (x=East, y=North, z=Up)
- `enuX`, `enuY`, `enuZ` — ENU axes as ECEF unit vectors (computed near top of main())
- `sunDir` — sun direction in ENU

---

## Current ocean code (to be replaced)

The existing implementation in Phase 3 (sea-level branch), after `surfColor` is computed:

```glsl
float oceanMask = textureGrad(earthSpecTex, uvSurf, uvd_dx, uvd_dy).r;
if (oceanMask > 0.5 && tHit < 0.0) {
    float wScale1 = 150.0;
    float wScale2 = wScale1 * 2.7;
    vec2  sc1 = vec2( 0.83,  0.55) * 0.0022 * pc.gmst;
    vec2  sc2 = vec2(-0.51,  0.87) * 0.0031 * pc.gmst;
    vec2  wuv1 = uvSurf * wScale1 + sc1;
    vec2  wuv2 = uvSurf * wScale2 + sc2;
    float e = 1.5 / 128.0;
    // ... 8 textureLod(noiseTex, ..., 0.0) samples for central-difference gradients ...
    float dEast  = (hE - hW) * 0.14;
    float dNorth = (hN - hS) * 0.14;
    vec3 surfUp = normalize(hitPt);
    vec3 waveN  = normalize(surfUp - dEast*vec3(1,0,0) - dNorth*vec3(0,1,0));
    // Blinn-Phong spec=300, Schlick Fresnel F0=0.02
    surfColor *= 0.75;  // darken diffuse
    surfColor += spec * vec3(1,0.97,0.9) * dayFrac;
    surfColor = mix(surfColor, vec3(0.12,0.28,0.50)*max(0.1,dayFrac), fresnel*0.5);
}
```

---

## Four problems to fix

### Problem 1: Wave scale too large
`wScale1=150` → one noise tile ≈ 267 km across the globe. Waves are easily visible from LEO
(550 km altitude). Also using `textureLod(..., 0.0)` bypasses mip filtering so there is no
distance falloff at all.

**Fix**: Distance-based wave LOD. Compute `float dist = tSeaLvl` (metres to ocean hit).
- Within ~2 km: full wave normals (4+ octaves, fine scale)
- 2–10 km: blended, coarser
- Beyond 10 km: no wave normals, flat Fresnel+specular only
Use ENU `hitPt.xy` (East/North offset in metres from observer) as the wave UV basis, not lat/lon.
This makes scale physically correct: `waveUV = hitPt.xy / waveLengthMetres`.
Use `texture()` (auto-LOD) or explicit distance-scaled LOD.

### Problem 2: Waves not visible from the surface
Two causes:
- UV scale is wrong for surface viewing: at 5 km ocean range, lat/lon UV span ≈ 0.00025, × 150 = 0.037 tiles, gradient ≈ 0 → flat normal → no glint.
- From grazing angle, specular requires wave normals tilted toward the viewer.

**Fix**: Use ENU `hitPt.xy` (metres) as wave UV. Wave scale ~20–100 m → `waveUV = hitPt.xy / 50.0`. Multiple octaves (at 50 m, 150 m, 500 m, 2000 m) give realistic near-to-far detail.

### Problem 3: Time tied to GMST
`pc.gmst` advances with simulated time, which can be time-warped. Waves should scroll at a constant real-world rate regardless of time warp.

**Fix**: Repurpose `pc.pad` (offset 76, float) as `pc.waveTime` = `glfwGetTime()` (seconds, wall-clock). Update `SatDrawPC` in `SatelliteSim.h` and fill in `recordDraw()`.

### Problem 4: Design — 3D waves and sky reflections

**Option A (recommended): Gerstner waves + reflected atmosphere**

*Gerstner wave height displacement:*
In the sea-level sphere intersection, instead of using `tBase.x` directly, refine the hit point
with Gerstner wave displacement along the sphere normal. Gerstner waves are analytic (sum of
`amplitude * sin(dot(k, pos.xy) - omega*t)`), cheap, and physically correct for deep water.

Integrate into the existing binary-search refinement or run a short secondary march (8–16 steps)
just in the ±waveAmp shell around sea level. This gives real 3D geometry: observer can see wave
peaks occlude wave troughs; correct parallax from low altitude.

Amplitude should scale with wind speed parameter (tune ~0.5–2 m for ocean swell). Beyond
~5 km, amplitude smoothly fades to 0 so the march doesn't waste steps on imperceptible waves.

*Sky reflection:*
After finding the ocean hit and computing `waveN`, compute:
```glsl
vec3 reflDir = reflect(dir, waveN);  // dir is incoming ray, waveN is surface normal
```
If `reflDir.z > 0` (reflecting skyward), evaluate a simplified atmosphere sample:
- Compute sun angle in reflection direction: `float cosRA = dot(reflDir, sunDir)`
- Rayleigh + Mie scatter along reflection ray: one atmosphere loop at N=6 samples
- Add sun disc contribution if `cosRA` is near 1.0
- This gives correct sky color, sun position, and horizon gradients in the reflection

If `reflDir.z < 0` (reflecting Earth back to itself): use `surfColor * 0.1` (dark, absorbs).

Mix the reflection with the base ocean color using Fresnel:
```glsl
vec3 reflColor = evaluateAtmosphere(reflDir);  // your sky loop, 6 steps
surfColor = mix(surfColor, reflColor, fresnel);
```

**Option B (simpler): 2D normals + approximate reflection**
Keep current 2D normal map approach but approximate the sky reflection as a
gradient based only on `reflDir.z` (elevation angle): lerp from horizon haze color to
zenith blue. Much cheaper, less correct.

**Recommendation**: Implement Option A. The atmosphere functions are already written and
tested. A 6-sample loop per ocean pixel is fast (much fewer than the 96-step terrain march
that runs every frame). Gerstner waves give real 3D geometry that's worth having.

---

## Implementation plan

1. **Push constant**: rename `pad[0]` → `waveTime` in `SatDrawPC` (SatelliteSim.h);
   fill with `(float)glfwGetTime()` in `recordDraw()` before the sky pass push constants.
   Update `pc.pad` → `pc.waveTime` in `sat_sky.frag` PC block.

2. **Wave normals (2D surface)**:
   - Replace lat/lon UV wave basis with ENU `hitPt.xy / wavelengthM`
   - 4 octaves at 50 m, 180 m, 650 m, 2300 m (ratio ≈ 3.6×)
   - Each octave: noise gradient via central differences, contribute at
     amplitude weighted by `exp(-dist / octaveRange)` so fine detail fades with distance
   - Use `pc.waveTime` for scroll
   - Cap total wave influence beyond 10 km via `mix(waveN, surfUp, smoothstep(5000,12000,dist))`

3. **Gerstner displacement** (optional but recommended):
   - 3–4 wave trains (different directions, wavelengths ~50–500 m)
   - Compute displaced sea level: `seaLevel = R_EARTH + sum(amplitude_i * sin(...))`
   - Refine `tSeaLvl` with a short 12-step march in range `[tBase.x - 3, tBase.x + 3]` (metres)
   - Only active within `dist < 5000 m`

4. **Sky reflection**:
   - At the ocean hit, compute `reflDir = reflect(dir, waveN)`
   - If `reflDir.z > 0`: run a 6-sample atmosphere loop (copy the existing loop structure
     from Phase 2, use `reflDir` as the view direction, start from `hitPt` in ENU)
   - Add sun disc contribution to the reflection
   - Mix: `surfColor = mix(surfColor * oceanAlbedo, reflColor, fresnel)`
   - `oceanAlbedo ≈ 0.05` (deep water absorbs most light)

5. **Fade at distance**:
   - `float distKm = tSeaLvl / 1000.0`
   - Gerstner: disable beyond 5 km
   - Fine octaves: fade beyond 2 km
   - Specular: always active (visible from orbit as glint)
   - Reflection: fade to approximate sky color beyond 30 km (avoid per-pixel atmosphere at orbit scale)

---

## Files to touch

| File | What changes |
|------|-------------|
| `src/simulations/SatelliteSim.h` | `SatDrawPC`: `float pad[2]` → `float waveTime; float pad;` |
| `src/simulations/SatelliteSim.cpp` | `recordDraw()`: set `pc.waveTime = (float)glfwGetTime()` |
| `shaders/sat_sky.frag` | PC block: `float pad` → `float waveTime`; replace ocean block ~line 578 |

No new textures, no descriptor changes, no pipeline changes needed.

---

## What NOT to do

- Do not touch the terrain march steps, binary search, or elevation constants
- Do not touch the depth buffer occlusion code (`gl_FragDepth` at end of main)
- Do not add a new render pass or descriptor bindings
- Do not modify the push constant total size (128 bytes, only rename pad[0])
- Build with `cmake --build build` and verify no shader compile errors before reporting done
