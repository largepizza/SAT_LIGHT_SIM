# CLAUDE.md

This file provides guidance to Claude Code when working with this repository.
The primary simulation is **SatelliteSim** (`src/simulations/SatelliteSim.h/.cpp`).
All other simulations (GameOfLife, Particles, Scene3DDemo) are legacy and rarely touched.

---

## Build Commands

```bash
cmake -B build -S .                           # configure (downloads deps via FetchContent)
cmake --build build                           # build + compile shaders + copy SPVs next to exe
cmake --build build --config Release          # release build
```

Run: `build/Debug/ShaderFun.exe`

Shaders: auto-detected glob (`shaders/*.vert|.frag|.comp`), compiled by `glslc`, copied as `shaders/*.spv`. New shader files are picked up automatically on next build.

**Do not launch or run the app yourself** (no `run` skill, no invoking the exe) to verify changes, especially UI behavior. Verifying UI/UX changes (opening the program, panning the camera, clicking through menus, etc.) is an involved manual process — the user runs and audits the app themselves after you build. Just build (and typecheck/compile-check) and report what changed; let the user test it.

**Requirements**: Vulkan SDK + `VULKAN_SDK` env var, CMake 3.20+, MSVC C++20.

---

## Architecture

Three layers (stable → frequently changed):

| Layer | Files | Role |
|-------|-------|------|
| Platform | `VulkanContext.h/.cpp` | All Vulkan boilerplate; exposes helpers |
| Framework | `App.h/.cpp`, `Simulation.h`, `UIRenderer.h/.cpp`, `AudioSystem.h/.cpp` | Window, frame loop, UI, audio |
| Simulation | `src/simulations/SatelliteSim.h/.cpp` | All active development |

### Frame Loop Order (`App::drawFrame`)
```
ui.beginFrame()          → resets Clay, saves prevMouseOverUI
sim->buildUI(dt, ui)     → Clay layout; camera look; mouse capture rects
sim->recordCompute(cmd)  → WASD movement; simTime advance;
                           CPU updatePositions() — sun/moon/obsECI/eci2enu/reflector targets only;
                           orbit rebake check (every 7 sim-days);
                           GPU dispatch 1: sat_orbit.comp (orbital mechanics + attitude normals);
                           buffer barrier satInputBuf (SHADER_WRITE → SHADER_READ, compute→compute);
                           vkCmdFillBuffer(glowBuf, 0) + barrier (clear per-frame histogram);
                           GPU dispatch 2: sat_flare.comp (lighting + visibility culling);
                           buffer barrier satVisibleBuf (SHADER_WRITE → SHADER_READ, compute→vertex)
vkCmdBeginRenderPass     → owned by App
sim->recordDraw(cmd)     → sky/ground background → satellite points → stars
ui.record(cmd)           → Clay → Vulkan quads/text/icons on top
vkCmdEndRenderPass       → owned by App
```

---

## Subsystem: UIRenderer / Clay

- `#define CLAY_IMPLEMENTATION` only in `UIRenderer.cpp`. All other files `#include "clay.h"` without it.
- `ui.input()` → `UIInput`: per-frame mouse/scroll/button state. `scrollY` positive = scroll up. `screenW/H` = window dims.
- `ui.mouseOverUI()` → **previous frame's** capture result. Read in `buildUI` to gate scene interaction.
- `ui.addMouseCaptureRect(x, y, w, h)` — call for every visible panel in `buildUI`.
- `Clay_Hovered()` only valid **inside** a `CLAY()` element body, not in the config struct.
- **One-frame hover lag**: store `Clay_Hovered()` in member bools; use those bools for colors the next frame.
- `CLAY_STRING(x)` requires a **string literal**. For runtime strings: `Clay_String{ false, (int32_t)strlen(buf), buf }` with a **member variable** buffer (Clay stores raw pointers read after `buildUI` returns).
- **Clip rule**: never put `.clip` on a floating container that also has `backgroundColor` — SCISSOR_START fires before RECTANGLE, hiding the background.
- Scrollable containers: `.clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}` on the content div.

### Icon Atlas
- `ui.loadIcons(ctx, paths, count)` — loads PNGs, packs into RGBA GPU atlas, rebinds descriptor. Call once on first frame (lazy init). Store `VulkanContext*` in your sim.
- Icons: `.image = {.imageData = (void*)(intptr_t)iconIdx}`. Renderer samples the atlas UV range for that index.
- Shader `mode`: `0.0` = solid rect, `1.0` = text glyph, `2.0` = icon sprite. Binding 1 is always valid (1×1 white placeholder at init).

### MSVC C++20 Designated Initializer Ordering
MSVC requires designators in declaration order:
- `Clay_LayoutConfig`: `sizing` → `padding` → `childGap` → `childAlignment` → `layoutDirection`
- `Clay_ElementDeclaration`: `layout` → `backgroundColor` → `cornerRadius` → `aspectRatio` → `image` → `floating` → `custom` → `clip` → `border` → `userData`
- `Clay_FloatingElementConfig`: `offset` → `zIndex` → `pointerCaptureMode` → `attachTo`

### Manual Hit-Testing
Clay does not expose element positions post-layout. Compute absolute positions from constants that exactly match Clay sizing declarations. Wrap labels in `CLAY_SIZING_FIXED` containers so layout width matches hit-test math.

---

## Subsystem: Controls / Keybinding Pipeline

**All interactive keys go through the `keybindings` vector.** The settings window and rebind UI are driven entirely from this vector — no extra wiring needed.

### `KeyBinding` struct
```cpp
struct KeyBinding {
    const char *action;  // display name in settings
    int  key;            // GLFW_KEY_*
    bool held;           // true = polled; false = event (pressed once)
    bool listening;      // true = waiting for rebind input
};
```

### `KB` enum (canonical indices)
```cpp
enum KB {
    KB_TOGGLE_UI  = 0,   // Tab    — event
    KB_PAUSE      = 1,   // Space  — event
    KB_SLOWER     = 2,   // ,      — event
    KB_FASTER     = 3,   // .      — event
    KB_REVERSE    = 4,   // R      — event
    KB_MOVE_BOOST = 5,   // LShift — held
    KB_MOVE_FINE  = 6,   // LCtrl  — held
    KB_CINEMATIC  = 7,   // LAlt   — event (toggle cinematic pan mode while RMB held)
    KB_COUNT      = 8,
};
```

### Adding a new control (complete checklist)
1. Add `KB_NEWNAME` before `KB_COUNT` in the enum
2. Add one line to `keybindings` in `init()`: `{"Display Name", GLFW_KEY_X, held, false}`
3. Bump `static_assert(KB_COUNT == N)` to the new count
4. Wire the action:
   - **Event** (`held=false`): `if (pressed(KB_NEWNAME)) { ... }` in `onKey()`
   - **Held** (`held=true`): `glfwGetKey(win, keybindings[KB_NEWNAME].key) == GLFW_PRESS` in `recordCompute()`

Settings display, rebinding, hover state, and `keyDisplayName()` all work automatically. `hovRebind[KB_COUNT]` is sized by the enum so no array changes are needed.

`keyDisplayName()` handles: letters, digits, Space, Tab, Enter, Esc, Bksp, modifier keys (LShift/RShift/LCtrl/RCtrl/LAlt/RAlt), F-keys (F1–F12), arrow keys, nav cluster (PgUp/PgDn/Home/End/Ins/Del), and common punctuation.

---

## Subsystem: Satellite Types

Each `SatelliteType` composes two surfaces + a diffuse floor:
- `primary` (`SurfaceSpec`) — always active
- `secondary` (`SurfaceSpec`) — optional; `weight=0` disables
- `diffuse` — constant Lambertian floor (always visible)
- `mirrorFrac` — fraction of primary that is near-perfect mirror; adds ultra-narrow spike on top of Phong lobe (MIRROR_BOOST=300×)

`SurfaceSpec`: `{AttitudeMode, specExp, weight}`

### AttitudeMode values
| Mode | surfN | Use case |
|------|-------|----------|
| `NadirPointing` | satNadir | Antenna/array face toward Earth (Starlink) |
| `SunTracking` | sunDirECI | Solar panels track sun (LEO Broadband, ISS) |
| `Tumbling` | spinning around random body axis | Debris, uncontrolled objects |
| `Perpendicular` | cross(surfN0, satNadir) | Secondary only — derived from primary normal |
| `AntiNadir` | -satNadir | Radiators facing deep space; brighter near horizon |
| `FlatMirror45` | normalize(sunDir + satNadir) | Flat mirror reflecting sunlight straight down |
| `TargetedReflector` | normalize(sunDir + toTarget) | Mirror aimed at nearest valid night-side ground target |
| `KnifeEdge` | roll around velHat; clamped ±80° | Starlink post-2020 roll-angle policy (Mallama 2023) |
| `SunPerp` | normalize(cross(sunDirECI, satNadir)) | Thermal radiator edge-on to sun; irr=0 always (correct thermal design — never receives direct sunlight). Visual contribution via diffuse. Used for AI1 datacenter radiators. |

`velHat` is computed in `sat_orbit.comp` from the orbital trig already in scope: `{-sinU·cosR - cosU·cosI·sinR, -sinU·sinR + cosU·cosI·cosR, cosU·sinI}` — already unit length for circular orbits.

### Satellite type catalogue (typeIdx)
| Idx | Name | Area (m²) | Primary attitude | Secondary | mirrorFrac |
|-----|------|-----------|-----------------|-----------|------------|
| 0 | Starlink | 10 | NadirPointing, spec=18 | — | 0.05 |
| 1 | LEO Broadband | 5 | SunTracking, spec=18 | — | 0.02 |
| 2 | GEO Comsat | 50 | SunTracking, spec=3 | AntiNadir, w=0.10 | 0.10 |
| 3 | ISS | 250 | SunTracking, spec=12 | AntiNadir, w=0.35 | 0.05 |
| 4 | SpaceX AI Sats | 600 | SunTracking, spec=25 | SunPerp, w=0.18 (radiators) | 0.01 |
| 5 | Reflect Mirror | 2376 | TargetedReflector, spec=200 | — | 0.97 |
| 6 | Debris | 1 | Tumbling, spec=6 | — | 0.03 |
| 7 | Starlink KE | 10 | KnifeEdge, spec=18 | — | 0.05 |

`crossSection = sqrt(crossSectionM2 / 10.0)` — so 10 m² → 1.0, 2376 m² → ~15.4.

### Satellite type data source
Types and constellations are loaded from `constellations.json` next to the exe. If the file is missing or malformed, `loadHardcoded()` provides the catalogue above as a fallback. The JSON schema is in `constellations.schema.json`.

### Adding a new satellite type
1. Add to `satTypes` in `constellations.json` (or `loadHardcoded()` as fallback)
2. No GPU struct changes needed; all fields map to existing `GpuSatInput` members
3. Reference the new typeIdx in a constellation entry

---

## Subsystem: Orbital Mechanics / Constellations

### Orbit distributions
- **Walker** — `numPlanes × perPlane` satellites, evenly spaced RAAN, random phase per plane
- **RandomShell** — random RAAN, random incl in [0, c.incl], jittered altitude, random tumble axis
- **Disk** — concentric rings in a single orbital plane (incl + raan). `alignTerminator=true` derives incl/raan from sunDirECI at J2000 epoch and precesses RAAN at SSO rate (kSSOPrecRate = 2π/year)

### ConstellationConfig field order
```cpp
// Walker:
{ name, altM, incl, numPlanes, perPlane, typeIdx, enabled, OrbitDistribution::Walker }

// Disk (extra trailing fields):
{ name, altM, incl, numPlanes, perPlane, typeIdx, enabled, OrbitDistribution::Disk,
  altJitterM, raan, alignTerminator, numRings, ringSpacingM }
```
- `perPlane` is **never** ignored — total = `numPlanes × perPlane` for all distributions
- `incl` is ignored when `alignTerminator=true`

### Adding a new constellation
1. Add a `ConstellationConfig` entry to `constellations.json` (or `loadHardcoded()`)
2. `hovConst` and `hovHighlightConst` are `std::vector<bool>` and auto-size to `constellations.size()` — no manual hover bool management needed
3. `MAX_SATELLITES = 10,000,000` — cap is generous; only relevant for very large test configs

### Current constellation roster (9 total, hardcoded fallback)
| Name | Sats | Alt (km) | Incl | Dist | TypeIdx |
|------|------|----------|------|------|---------|
| Starlink Gen1 | 4,392 | 550 | 53° | Walker | 0 |
| Starlink Gen2 | 30,480 | 525 | 53.2° | Walker | 7 (KnifeEdge) |
| OneWeb | 648 | 1,200 | 87.9° | Walker | 1 |
| Amazon LEO | 7,742 | 630 | 51.9° | Walker | 1 |
| Guowang | 13,920 | 508 | 85° | Walker | 1 |
| ISS | 1 | 408 | 51.6° | Walker | 3 |
| SpaceX AI Sat | 20,000 | 575–1,925 | SSO | Disk+terminator, 10 rings | 4 |
| Reflect Orbital | 1,000 | 500 | SSO | Disk+terminator, 10 rings | 5 |
| Space Junk | 3,000 | ~1,000 | random 0–180° | RandomShell | 6 |

CPU `updatePositions()` is now **O(1)** — it only updates sun/moon/obsECI/eci2enu and uploads `reflectorTargetsBuf`. All orbital mechanics run on GPU via `sat_orbit.comp`.

### SSO precession model (alignTerminator=true)
Inclination from J2 formula: `cos(i) = -kSSOPrecRate / (1.5 × n × kJ2 × (Re/a)²)`
RAAN anchored at **sim-start** using `sunDirECI` (set by `updatePositions()` before `initConstellation()`): `raan_start = atan2(sunDirECI.x, -sunDirECI.y)`. GPU formula: `liveRaan = raan_start + kSSOPrecRate × (simTime − t_start)`. Anchoring at sim-start avoids the ~3° obliquity-driven phase error that accumulates when extrapolating from J2000 to a solstice epoch.

---

## Subsystem: GPU Orbital Pipeline

All per-satellite orbital mechanics and attitude computation runs on the GPU. The CPU only manages the small reflector targets buffer and triggers a rebake when needed.

### Two-dispatch pattern (recordCompute)
```
sat_orbit.comp dispatch   → reads satOrbitBuf; writes satInputBuf + mirrorNormalsBuf
barrier satInputBuf       → SHADER_WRITE → SHADER_READ, compute→compute
vkCmdFillBuffer(glowBuf)  → zeros the glow histogram for this frame
barrier glowBuf           → TRANSFER_WRITE → SHADER_READ|SHADER_WRITE, transfer→compute
sat_flare.comp dispatch   → reads satInputBuf; writes satVisibleBuf + glowBuf (atomicMax)
barrier satVisibleBuf     → SHADER_WRITE → SHADER_READ, compute→vertex
```

### Buffers
| Buffer | Memory | Lifetime | Updated by |
|--------|--------|----------|------------|
| `satOrbitBuf` | device-local | uploaded once; rebaked every 7 sim-days | `uploadSatOrbits()` |
| `satInputBuf` | device-local | per-frame | `sat_orbit.comp` |
| `satVisibleBuf` | device-local | per-frame | `sat_flare.comp` |
| `mirrorNormalsBuf` | device-local | persistent (slew state) | `sat_orbit.comp` read+write |
| `reflectorTargetsBuf` | host-visible, mapped | per-frame | `updatePositions()` CPU |
| `glowBuf` | host-coherent, mapped | per-frame | `sat_flare.comp` write; App reads back |

### Orbit rebake
`kOrbitRebakeDays = 7`. Each `GpuSatOrbit` bakes `u0 = fmod(orig_u0 + meanMot × epochT0, 2π)` so the shader only adds `meanMot × deltaT` where deltaT < 7×86400 s. Float ULP at that scale ≈ 0.07 s, well within tolerable orbital error. `uploadSatOrbits()` auto-triggers in `recordCompute()` when `|simDayJ2000 - orbitEpochDay| >= 7`.

### simTime representation
Split into `simDayJ2000` (int64_t days) + `simSecInDay` (double, re-based to [0, 86400) each frame). Avoids accumulated float precision loss when a large J2000 base is added to a small per-frame delta. The shader receives `deltaT = float((dDays × 86400) + dSec)` where dDays < 7 (ensured by rebake).

### GpuSatOrbit layout (112 bytes, std430)
All plain floats/uints — no vec3 — so C++ struct packing matches GLSL std430 with no padding.
Must match `SatOrbit` in `sat_orbit.comp` exactly.
```
[ 0] raan, u0, R_sat, meanMot
[16] cosI, sinI, cosRaan, sinRaan
[32] tumbleRate, tumblePhase, alignTerminator, tumbleAxisX
[48] tumbleAxisY, tumbleAxisZ, primaryAttitude (uint), secondaryAttitude (uint)
[64] baseColorR, baseColorG, baseColorB, crossSection
[80] specExp0, specExp1, w1, diffuse
[96] mirrorFrac, constIdx (uint), pad0, pad1
```
`static_assert(sizeof(GpuSatOrbit) == 112)` — do not change field order without updating both structs.

### GpuSatVisible layout (32 bytes, std430)
Output of `sat_flare.comp`; read by `sat_point.vert`.
```
[ 0] skyDir (vec3) + flareIntensity (float)  — ENU unit vector + intensity [0,1+]
[16] baseColor (vec3) + angularSize (float)  — tint + point sprite size hint (pixels)
```
`static_assert(sizeof(GpuSatVisible) == 32)`

### Push constants

**SatOrbitPC** (96 bytes) — sat_orbit.comp:
```
enuX (vec4), enuY (vec4), enuZ (vec4)  — ECI→ENU basis, offsets 0/16/32
sunDirECI (vec3), deltaT (float)       — offset 48/60
obsECI (vec3), satCount (uint)         — offset 64/76
highlightMask (uint), enabledMask (uint), simDt (float), pad (float)  — offset 80/84/88/92
```

**SatFlarePC** (128 bytes) — sat_flare.comp:
```
enuX (vec4), enuY (vec4), enuZ (vec4)  — offsets 0/16/32
sunDirECI (vec3), satCount (uint)      — offset 48/60
obsECI (vec3), elevCutoff (float)      — offset 64/76
brightnessScale, daySuppression, mirrorBoost, visThresh, highlightFlare,
pad2, moonSuppression, pad0            — offsets 80–108
moonDirECI (vec3), pad1 (float)        — offset 112/124
```
`pad2` was `lightPollution` — superseded (session 26) by the directional `lightDomeBuf` SSBO
(binding 3 in the sat_flare.comp descriptor set, 8 floats, host-visible/mapped), which doesn't
need push-constant space. See "Subsystem: Light Pollution Dome" below.

**SatDrawPC** (112 bytes) — sat_point.vert + both sky shaders:
```
skyView (mat4)                          — offset 0
fovYRad, aspect, pad[2]                 — offsets 64/68/72/76
sunDirENU (vec4) — xyz=dir, w=sin(el)  — offset 80
moonDirENU (vec4) — xyz=dir, w=illum   — offset 96
```

---

## Subsystem: TargetedReflector / Mirror Ground Targets

Mirrors in `TargetedReflector` mode aim at the nearest valid night-side ground target. All per-satellite selection and slew computation runs in `sat_orbit.comp`.

### Target generation (once at init)
`kNumReflectorTargets = 201` random ECEF unit vectors, uniformly distributed on sphere.
Last slot (index 200) is fixed at the observer spawn point (67°S, 67°W) — always aimed here when in darkness.

### Per-frame CPU update (updatePositions)
Rotates targets from ECEF to ECI via GMST = `kOmegaEarth × t`.
Marks each target valid (night-side only): `dot(normalize(targetECI), sunDirECI) < 0`.
Uploads `GpuReflectorTarget[201]` to `reflectorTargetsBuf` (host-visible, mapped).

### Per-satellite (GPU, sat_orbit.comp)
Scans all 201 targets, picks nearest by `dot(satZenith, normalize(targetECI))`.
Mirror normal = `normalize(sunDirECI + toTarget)` — reflects sunlight toward target by half-vector identity.
Falls back to FlatMirror45 (straight down) if no valid targets exist.
Slew state persists in `mirrorNormalsBuf` (device-local, read+write in place each dispatch).

### Mirror slew rate
`kMirrorRotRateDegPerSec = 1.0f` degrees per **simulated** second — consistent across all time warp levels.
Zero-vector in `mirrorNormalsBuf` = uninitialized; snaps to target on first frame, then slews at the clamped rate.

---

## Subsystem: Photometry / Shader Constants

Photometry values are **runtime members** on `SatelliteSim`, synced to `SatFlarePC` each frame. They are persisted in `settings.json` and adjustable in the settings window.

| Member | Default | Description |
|--------|---------|-------------|
| `brightnessScale` | 1.0 | global flux multiplier |
| `daySuppression` | 500.0 | sky background suppression ratio (sun) |
| `mirrorBoost` | 300.0 | mirror peak multiplier (MIRROR_BOOST) |
| `visThresh` | 0.0 | visibility cull threshold |
| `highlightFlare` | 0.05 | fixed flare for highlight/census mode |
| `moonSuppression` | 4.0 | sky background suppression ratio (moon) |
| `lightPollutionGain` | 1.0 | multiplies the light-pollution dome at its source — see "Subsystem: Light Pollution Dome" |
| `extinctionCoeff` | 0.25 | atmospheric extinction, magnitudes per airmass — see "Subsystem: Atmospheric Extinction" |

`effectFlare = flare / (1 + (dayBright × daySuppression + moonBright × moonSuppression) × atmFrac)`,
then `×= extinction` (airmass), then `×= (1 − domeVal × 0.85)` (light pollution)
`magnitude = kMagRef - 2.5 × log10(effectFlare / kMagRefFlare)` where `kMagRef=6.0`, `kMagRefFlare=0.008`

`dayBright`/`moonBright` are elevation-ramp scalars (squared linear, sun/moon dot observer-zenith)
computed once per frame — **uniform across the sky, not per-satellite-direction**. This is an
accepted simplification for both (unlike light pollution below, neither has been made directional).
`moonBright` additionally omits the near-moon sky-brightening halo (real moonlight scatters more
strongly close to the moon's disc) — not built, no current plan to.

Stars (`SatelliteSim::updateStars`, CPU-side) apply the same three suppression sources
independently, with their own fixed (non-slider) response caps: `kStarPollutionMaxDim=0.85`,
`kStarMoonMaxDim=0.9`. Day suppression for stars is `nightFactorEff` (sun-elevation ramp), not
`dayBright`/`daySuppression` — a separate, older formula; the two were never unified.

## Subsystem: Light Pollution Dome

Session 26 replaced a single scalar (city brightness at the *observer's own* lat/lon — correct
about moving with the observer, wrong about being uniform across every direction of the sky) with
a 16-azimuth-sector dome, interpolated between sector centers, brighter near the horizon toward
nearby cities and fainter elsewhere — consumed identically by both satellites and stars.

**`SatelliteSim::updateLightPollutionDome()`** (CPU, called each frame in `recordCompute()` right
before `updateStars()`): for each of 16 sectors (22.5° each, bearing clockwise from North —
independent of `sat_flare.comp`'s unrelated `GlowBuf` 8-sector `azBin`, decoupled on purpose),
samples `earthNightCpu` at 4 radii (2/8/20/45 km) along that bearing using a flat-Earth
tangent-plane lat/lon offset (adequate at this scale), combined via **weighted max** (a single
nearby bright city should dominate that direction, not get averaged down by darker samples at
other radii in the same sector) with `exp(-D/20000)` distance weighting. The 2 km near sample
exists because the observer's own position can sit inside a bright pixel while every 8+ km ring
around it is already dark countryside (small/isolated towns) — without it the dome could miss the
pollution source entirely, the direct analog of the old scalar's distance-0 sample. Response curve
(`kNightFloor`/`kCityCompressK`) and the observer's own altitude falloff (`exp(-obsHeight/3000)`)
match the pre-session-26 scalar's constants exactly — only the sampling geometry changed. Result
scaled by `lightPollutionGain` (settings-window slider "Pollution gain", default 1.0, user-widened
range) applied once here at the source — **intentionally left unclamped**, not `clamp`ed to `[0,1]`
— so satellites and stars stay coherently scaled by construction (same array). A 5-tap circular
blur (`[0.1, 0.2, 0.4, 0.2, 0.1]`, ~±45°) then smooths the 16 raw per-sector values before storing:
each sector is a single bearing ray, so a real city's edge (which doesn't line up with 22.5° sector
boundaries) could put a bright sector directly next to a dark one — sampling noise, not genuine
geography, and the direct cause of "stars/satellites suddenly get much brighter" pops reported
when panning across a sector boundary near the horizon (worst there because `elevFalloff` is
largest at the horizon, fully exposing the noise). Result: `lightDomeAz[16]`, a CPU member array.

**Delivery:** `lightDomeAz` is memcpy'd into `lightDomeBuf` (host-visible/coherent, 16 floats,
binding 3 in the sat_flare.comp descriptor set — same `reflectorTargetsBuf`-style "CPU writes,
GPU reads this frame, single frame in flight" pattern, no barrier needed) for `sat_flare.comp`.
`updateStars()` reads the `lightDomeAz` CPU array directly, no upload round-trip needed.

**Per-consumer lookup** (both `sat_flare.comp` and `updateStars()` compute this the same way, GLSL
and C++ mirrors of each other): rather than a hard `azBin` lookup, interpolates between the two
nearest sector *centers* — `secF = bearing/22.5° - 0.5`, `sec0 = floor(secF)`,
`domeAz = mix(lightDomeAz[sec0], lightDomeAz[sec0+1], frac(secF))` (both indices wrapped mod 16).
Hard-binning (even at 16 sectors) showed visible blocky transitions over wide, fairly uniform
bright regions (e.g. flying over Europe) — the interpolation, not the sector count, is what fixes
that. Then `elevFalloff = 0.35 / (max(skyDir.z, 0) + 0.35)` (1.0 at the horizon, ~0.26 at zenith —
city glow hangs low in the sky, not overhead). **The only clamp is here**, after `elevFalloff`:
`domeVal = clamp(domeAz * elevFalloff, 0, 1)` — clamping `lightDomeAz` itself upstream was a real
bug (fixed same session): it let `elevFalloff` (≤1 off the horizon) silently cap the *effective*
max well below 1.0 at every non-horizon angle, no matter how high `lightPollutionGain` went, so
gain past the point where it first saturated the pre-`elevFalloff` value (~5) looked identical to
gain=500. `domeVal` feeds the existing `1 - domeVal × kPollutionMaxDim` dimming multiplier
unchanged (`kSatPollutionMaxDim` = 0.85 in `sat_flare.comp`, `kStarPollutionMaxDim` = 0.99 in
`updateStars()`, both user-tuned — still a hard ceiling on max dimming regardless of gain).

**Not built:** the elevation falloff shape is a fixed analytic curve, not itself sampled/measured —
a true 2D (azimuth × elevation) dome would need real atmospheric-scattering-height modeling, judged
not worth the complexity over the fixed-curve approximation.

## Subsystem: Atmospheric Extinction

Session 26 follow-up: the light-pollution dome's `elevFalloff` was, until this was added, the
*only* term anywhere that varied a star's or satellite's brightness by its own viewing elevation —
there was no real horizon-dimming baseline, which is part of why the dome's directional noise (see
above) read as unsubtle: nothing else was smoothly dimming things toward the horizon for it to
modulate on top of.

**Formula** (identical in `sat_flare.comp` and `updateStars()` — a star and a satellite at the same
elevation must dim by the same amount, since this represents real atmospheric transmission, not a
stylized brightness knob): Kasten & Young 1989 airmass approximation,
`airmass = 1 / (sin(el) + 0.50572 × (elDeg + 6.07995)^-1.6364)` — stays finite down to the true
horizon (elDeg=0 → airmass≈38), unlike the naive `1/sin(el)` which diverges to infinity. Then
`extinctMag = extinctionCoeff × (airmass - 1) × atmFrac` (magnitudes of dimming beyond the zenith
baseline; `atmFrac`-gated since an orbiting observer has no atmospheric column along the line of
sight regardless of apparent "elevation" in their local frame) and
`extinction = 10^(-0.4 × extinctMag)`, multiplied directly into `effectFlare`/star `intensity`.

**Tunable:** `extinctionCoeff` (magnitudes per airmass; ~0.2-0.3 is typical clear-sky sea-level;
default 0.25), settings slider "Extinction". Reuses `SatFlarePC`'s `pad2` slot (the one freed by
`lightPollution`'s move to `lightDomeBuf`) rather than growing the struct — stars read the same
`extinctionCoeff` C++ member directly, no separate push-constant path needed for the CPU side.

`MIRROR_BOOST = 300` — peak multiplier for near-perfect mirror alignment. `mirrorExp = max(specExp0 × 300, 8000)` gives sub-degree angular width (matches solar disc ~0.26°).

---

## Subsystem: GpuSatInput Layout (80 bytes, std430)

Written by `sat_orbit.comp`, read by `sat_flare.comp`.

```
[  0] eciRelPos (vec3) + range (float)
[ 16] surfN0    (vec3) + elevation (float)   — primary surface normal; elevation = -π/2 for below-horizon/disabled
[ 32] surfN1    (vec3) + specExp0 (float)    — secondary surface normal
[ 48] baseColor (vec3) + specExp1 (float)
[ 64] crossSection + w1 + diffuse + mirrorFrac (float×4)
```

`static_assert(sizeof(GpuSatInput) == 80)` — do not change field order without updating both the C++ struct and the GLSL `SatInput` struct in `sat_flare.comp`.

Below-horizon and disabled satellites write `elevation = -π/2` and return early. `sat_flare.comp`'s horizon cull (`elevation < -0.01 rad`) discards them at zero cost.

---

## Subsystem: Sky Glow SSBO

`sat_flare.comp` writes a spatial histogram + per-satellite flare list each frame → `sat_sky.frag` reads them.

### GpuGlowBuf layout (std430)
```cpp
static constexpr int kGlowBins  = 64;   // 8 azimuth × 8 elevation cells (45° × 11.25°)
static constexpr int kMaxFlares = 8;    // per-satellite lens-flare slots

struct GpuGlowBuf {
    uint32_t bins[kGlowBins];           // atomicMax(floatBitsToUint(effectFlare)) per bin — wide Gaussian glow
    uint32_t flareCount;                // number of entries claimed (capped at kMaxFlares)
    uint32_t flarePad[3];
    glm::vec4 flareEntries[kMaxFlares]; // xyz=ENU dir, w=effectFlare — spiky corona + lens artifacts
};
// sizeof = kGlowBins*4 + 16 + kMaxFlares*16
```
`static_assert(sizeof(GpuGlowBuf) == kGlowBins * 4 + 16 + kMaxFlares * 16)`

`kGlowBins` and `kMaxFlares` must match constants in `sat_sky.frag`. `glowBuf` must be zeroed with `vkCmdFillBuffer` before each `sat_flare.comp` dispatch (floatBitsToUint(0.0) == 0u, so fill value 0 correctly marks bins empty).

---

## Subsystem: VulkanContext Helpers

```cpp
ctx.device, ctx.physicalDevice, ctx.renderPass, ctx.swapExtent, ctx.swapFormat
ctx.graphicsQueue, ctx.commandPool
ctx.loadShader("shaders/foo.spv")
ctx.createBuffer(size, usage, props, buf, mem)
ctx.createImage(w, h, fmt, usage, img, mem)
ctx.beginOneTimeCommands() / ctx.endOneTimeCommands(cmd)
ctx.imageBarrier(cmd, img, srcAccess, dstAccess, oldLayout, newLayout, srcStage, dstStage)
ctx.findMemoryType(filter, props)
```

Key Vulkan design decisions:
- Single command buffer, single frame in flight
- `VK_ACCESS_SHADER_READ_BIT` + `VK_PIPELINE_STAGE_VERTEX_SHADER_BIT` for compute→vertex SSBO barriers (not `VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT`)
- Compute→compute SSBO barriers use `VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT` on both sides
- `onResize` must recreate graphics pipelines (viewport baked in); compute pipelines are viewport-independent
- `glowBuf` is `HOST_COHERENT` so CPU can read back peak flare for magnitude UI without an explicit flush; previous frame's data is safe to read at the start of `recordCompute` (single frame in flight means queue is idle)

---

## Subsystem: Persistent Settings

`settings.json` is written next to the exe on settings-window close and on `cleanup()`, loaded in `init()` after `initConstellation()`.

Persisted fields: photometry params, `ui_scale`, settings window position, audio volumes, camera orientation (`az_deg`, `el_deg`, `fov_y_deg`), observer lat/lon, time scale index, keybindings (action → GLFW key code), constellation `enabled` + `highlight` state per name.

If the file is missing (first run) all defaults are used silently.

---

## Subsystem: GPU Performance Profiling

Built session 29 to replace guesswork ("N_VIEW is probably the bottleneck") with real
measurement — used to find and fix the terrain step-count bug and the aurora resolution/noise-bake
wins documented under "Active Development" above. Four pieces:

**In-app GPU timestamp queries** (`VulkanContext`): a 7-slot `VK_QUERY_TYPE_TIMESTAMP` pool.
Single frame in flight, so results are resolved in `App::drawFrame` right after the fence wait —
no stall. Slot layout is a shared contract: App.cpp writes 0 (frame start), 5 (satellite+star draw
done), 6 (UI overlay done); `SatelliteSim` writes 1-3 in `recordCompute` (cloud march / orbit
compute / flare compute done) and 4 in `recordDraw` (sky background draw done — this is what
isolates the fullscreen atmosphere/terrain/ocean shader's own cost from the satellite/star point
draws that follow it in the same render pass; they used to be one fused bucket).
`SatelliteSim::updateGpuTimingStats()` EMA-smooths the six deltas into `gpuMsSmoothed[6]`,
displayed in Settings → Display → "GPU FRAME BREAKDOWN" (one-frame-stale, same pattern as
`peakMagnitude`).

**Perf knockout toggles**: `debugDisableMask` (uint32, in both `SatDrawPC` and `CloudMarchPC` — see
their own entries above) is a profiling-only bitmask. Six checkboxes in Settings → Display →
"KNOCKOUT PROFILING" each disable one shader block — terrain march, atmosphere loop, sun optical
depth (`optDepth`, called from 4 sites — zeroing it there is a single early-return in the function
itself, not 4 separate call-site edits), ocean sky reflection, airglow red, aurora — each with a
mathematically-safe zero/no-op fallback (e.g. terrain-skip leaves `tHit=-1`, the same value the
"no hit" path already produces). Default mask 0 is bit-identical to normal rendering. Use this to
isolate one block's real GPU cost via before/after `gpuMsSmoothed` deltas, without a GPU capture
tool — bit assignments: 1=terrain, 2=atmosphere, 4=sunOD, 8=oceanRefl, 16=airglowRed, 32=aurora.

**`perf_profiles/profile_log.jsonl`**: the "Save Snapshot" button (same panel) appends one JSON
record per press — GPU timing breakdown, resolution, observer lat/lon/altitude, sim time, active
knockout mask, GPU device name, quality settings. JSON Lines (not a JSON array) so the log grows by
simple appending across sessions/restarts. `SatelliteSim::savePerfSnapshot()`.

**`tools/perf_analysis/`**: a small Python toolkit (gitignored `.venv`, `requirements.txt`: pandas
+ matplotlib) — `analyze_profile.py` reads the JSONL log and reports GPU cost by resolution bucket,
per-megapixel cost (flat across resolutions = purely resolution-bound), a matched-altitude
resolution ratio (isolates the resolution effect from confounding scene/altitude changes in the
same dataset — see the script for why raw correlation isn't enough), a knockout-toggle cost
summary, and Pearson correlations against scene variables, plus two PNG plots. Re-run this any
time a new round of snapshots is captured: `tools/perf_analysis/.venv/Scripts/python.exe
tools/perf_analysis/analyze_profile.py`.

See `TERRAIN_PLAN.md` session 29 log for the full narrative — what was measured, what was
concluded, and which prior assumptions (the session-24 transmittance-LUT guess) it overturned.

---

## Subsystem: Resolution Scaling

Settings → Display → "Render scale" (50%-100%, default 100%, top of the tab — a user-facing perf
option, not a debug tool). Only the sky/terrain/ocean/cloud-composite background scales;
satellites, stars, and UI always render at native resolution, no exceptions — the explicit design
goal, given the earlier-session concern about losing tiny satellite point fidelity to a whole-
frame downscale.

**At the default (`renderScale==1.0`) this is a no-op — the code path is identical to before the
feature existed.** Below 100%, `SatelliteSim::recordPrePass` (a new `Simulation` interface hook,
default no-op, so the other simulations needed zero changes) renders the background into a low-res
offscreen target and blits it (`vkCmdBlitImage`, linear filter) directly into the swapchain image
*before* the main render pass opens. The main render pass then uses `ctx.renderPassLoad` instead of
`ctx.renderPass` — a second render pass object (`Simulation::activeRenderPass`, another new hook,
default `ctx.renderPass`) with the SAME attachment formats (so it stays compatible with the same
`ctx.framebuffers` — render-pass compatibility only requires matching format/sample-count, not
matching load/store ops) but LOAD instead of CLEAR for color, so the pre-pass's blit survives into
the frame instead of being cleared away.

**Depth is deliberately not blitted** — depth-format blit support isn't spec-guaranteed the way
color-format blit support effectively always is, a real portability risk specifically on the
lower-end hardware this feature targets. Consequence, accepted: satellites/stars are not occluded
by terrain while `renderScale<1.0` (a satellite that should hide behind a mountain may show
through). Only applies below 100%.

**`gl_FragCoord` gotcha — read this before adding any new `gl_FragCoord`-based lookup to
`sat_sky.frag`.** `gl_FragCoord.xy` is relative to whatever framebuffer the CURRENT draw call
targets, not always the full swapchain — a real bug shipped and was fixed same-session: the cloud
composite sample divided `gl_FragCoord.xy` by `textureSize(cloudTargetA,0)*2.0` (an assumed full-
res constant, since `cloud_march.comp`'s own dispatch is unaffected by `renderScale`), which
silently broke the moment the background could render into a smaller offscreen target — clouds
drifted off-center, fully distorted at 50%. Fixed via a new `SatDrawPC` field, `screenSizePx`
(THIS draw's actual target size — `skyLowResExtent` when pre-rendering scaled,
`ctx.swapExtent` everywhere else) — any `[0,1]`-normalized UV derived from `gl_FragCoord` must
divide by `pc.screenSizePx`, never an assumed constant. (A fixed-frequency noise seed like the
terrain-march jitter lookup, `gl_FragCoord.xy * (1.0/128.0)`, is fine as-is — no total-resolution
assumption baked in, not a normalized UV.)

`SatDrawPC` grew 132→144 bytes for `screenSizePx` — needed an explicit `pad0` float first, since
GLSL's `push_constant` block requires 8-byte alignment for a `vec2` that C++ doesn't insert
automatically. `buildSatDrawPC(ctx, targetExtent)` factors out the shared push-constant fill
between `recordPrePass` and `recordDraw` so `aspect` (always the true screen aspect) and
`screenSizePx` (always this draw's real target) can't drift apart between the two call sites.

See `TERRAIN_PLAN.md` session 29 log for the full design writeup and the bug's root-cause
narrative.

---

## Fixed Simulation State

**Start epoch**: UTC 2036-06-21 00:00:00 → J2000 seconds = 1,150,891,200 (stored split: day 13,320 + 43,200 s)
**Observer**: 67°S 67°W → ECEF `obsDir = {0.1527, -0.3596, -0.9205}`, facing north
**Moon phase offset**: `kMoonPhaseOffsetRad = 3.916 rad` → originally calibrated for 2026-03-30; moon phase at new epoch will differ

---

## Active Development: Earth / Terrain Rendering

See `TERRAIN_PLAN.md` in the project root for the full step checklist and session log.
Read it at the start of any terrain-related session before making changes.

**Current state (as of 2026-07-17, session 29):**
- Steps 1, 2, 3, 4, 5, 5b, 6, 8 complete; C1–C8, C13, C15, C16 complete
- Phase E in progress (C13–C16: Cirrus rework, Anvil, Airglow, Aurora), sequenced ahead of
  C9/C11/C12. Full spec in `TERRAIN_PLAN.md`.
- **Cirrus (C13):** own standalone `cirrusMarch()` in `sat_sky.frag`, NOT a second `cloudMarch`
  call — `cloudMarch` already merges `layers[0]`/`[1]` (2-11km) into one low/mid shell, so there
  was no separate volumetric band to extend. Thin shell (700m) at `layers[1].shellAltM`,
  anisotropic streaks via a fixed global wind-axis compression (`cloud.cirrusWindAngle`/
  `cirrusStretch`, repurposed from the UBO's former `pad1`/`pad2`) — NOT a per-sample tangent
  decomposition (that's a no-op: the noise argument is purely radial from its own tangent frame).
  Sun-only lighting matching `evalCloudLayer`'s formula so it colour-matches the flat paste it
  crossfades against. See `TERRAIN_PLAN.md` session 21 log for the full writeup.
- **Airglow (C15):** three emissive bands (green 96km, sodium 90km, red 275km) gated by per-sample
  geographic day/night, not observer's. Green+sodium accumulate inside the existing `N_VIEW`
  atmosphere loop for free (their peaks fall inside its ~100km ceiling) and stay in `sat_sky.frag`.
  Red originally needed its own small 16-step supplemental march out to `R_EARTH+500km` since its
  peak/half-width sit well past that ceiling — extending the primary loop's far bound for one band
  would have coarsened the near-surface Rayleigh/Mie sampling everything else depends on; that red
  march itself moved to `cloud_march.comp` in session 29 (half-res, alongside aurora — see below),
  though the underlying peak/width/color constants and the day/night gating logic are unchanged.
  `CloudParams` UBO grew 176→192 bytes (all 3 pad slots were already consumed by C13) for 4 new gain
  fields (`airglowGain` master + per-band); peak altitude/width/color are hardcoded physical
  constants, not UBO fields. Reuses the analytic `warpPerlin3` noise (same one `cloudWarpOffset`
  uses) for horizontal patchiness — no new texture/binding. See `TERRAIN_PLAN.md` session 22 log for
  the original writeup, session 29 log for the move.
- **Raymarch-from-inside-a-volume fix (session 22):** `raySphere` reformulates `c = dot(ro,ro)-r*r`
  as `c = (|ro|-r)*(|ro|+r)` — the naive form catastrophically cancels at R_EARTH scale (~1e13
  float32 magnitude) exactly at grazing/near-tangent rays, i.e. every horizon, across all 29 call
  sites. Also: any shell march must classify the observer as below/inside/above the shell (keyed on
  `obsEffH`) rather than assuming a fixed forward root — `cloudMarch`/`cirrusMarch` already did this;
  the new airglow red-band march didn't, and broke (bright zenith band + horizon seams) once the
  observer flew (via the uncapped "Raise Elevation"/Q control) into or above the shell and looked
  outward, where the "always below" forward root goes negative. Fixed to match the established
  pattern. Any future shell march (Aurora/C16) needs this from the start — see
  `TERRAIN_PLAN.md` session 22 log.
- **Cloud march perf (session 22 follow-ups):** (1) `cloudMarch`'s C8 altitude-stratified stepping
  uncapped the real 3D step length for oblique rays (up to 50× the vertical step) — this is what
  made "clouds viewed from the side" undersample and band; now capped at a fixed `kCloudMaxStepM =
  250` (meters, not a multiple of the vertical step — see comment on why that matters at high
  `marchSteps`). (2) `cloud.lightSteps` (the "Light steps" slider) was declared but never read
  anywhere — the sun self-shadow cone hardcoded `N_CONE = 6` regardless; now wired up, and it's the
  dominant per-inCloud-sample cost. (3) That shadow cone is now also distance-gated
  (`cloud.shadowMaxDistM`, camera-relative) so far/orbital clouds skip it almost entirely, which
  paid for raising the render-distance cap (`cloud.maxRenderDistM`, replaces a hardcoded 80km) to
  reduce horizon pop-in. `CloudParams` grew again, 192→208 bytes. See `TERRAIN_PLAN.md` session 22
  log (multiple entries) for the full history.
- **Half-resolution cloud compute pass (session 23) — `cloudMarch()`/`cirrusMarch()` no longer live
  in `sat_sky.frag`.** They moved to `shaders/cloud_march.comp`, a new compute shader dispatched
  once per frame in `recordCompute()` at half `ctx.swapExtent` (1/4 the pixels). `sat_sky.frag`
  samples two `RGBA16_SFLOAT` targets (bindings 10/11) instead of marching per full-res pixel.
  Restructured (not just relocated): the compute-shader copies return an `(A, B)` affine-composite
  pair instead of mutating `color` in place, so cirrus-then-cloud combine into one exact
  `(A_total, B_total)` algebraically. No terrain data in the compute shader — `sat_sky.frag` does a
  post-hoc terrain-occlusion suppression using its own accurate `tSurface` against the sampled
  occlusion distance (exact for full occlusion, not for mid-shell partial truncation — accepted
  approximation). New `CloudMarchPC` push constant, new `cloudMarchDescSet` (7 bindings), 2 new
  `skyDescSet` bindings. See `TERRAIN_PLAN.md` session 23 log for the full design (why two targets,
  the barrier sequencing, the `init()` ordering constraints — several real mistakes were caught and
  fixed during design review before any code was written, don't repeat them).
- **Terrain-bleed bug fix + `cloudShadowFactor()` removed (session 23 follow-ups):** the terrain-
  suppression gate above initially used the opacity-gated `tCloudOcclude` (≥90% opaque only, meant
  for satellite depth), so most non-solid cloud rendered through terrain regardless of depth — a
  real bug, not the documented approximation. Fixed with a second, always-valid entry distance
  (`tEnterOut` from both march functions, combined via `min()`) stored in Target B's alpha;
  `cloudBlock` (displaced from that slot) is now derived from Target B's RGB instead. Separately:
  Release-build FPS testing showed the half-res compute move hadn't changed SURFACE performance at
  all (unchanged across the whole session, through every cloud-march fix) — `coverage=0` testing
  confirmed clouds were still the dominant surface cost anyway, pointing at `cloudShadowFactor()`
  (full-res cloud-shadow-on-terrain/ocean, untouched all session) as the real bottleneck. Removed
  outright per user decision (cloud shadowing on terrain isn't in use) rather than optimized — its
  `CloudParams` UBO slot reverted to `pad0`. See `TERRAIN_PLAN.md` session 23 log for both fixes.
- **Terrain/ocean/atmosphere perf follow-up (session 24):** fixed a real regression — the terrain
  march's altitude-scaled step count was `mix(320.0, 320.0, ...)` (a no-op, always paid the
  LEO-tuned 320-step budget at ground level too), restored to `mix(196.0, 320.0, ...)` matching its
  own comment. Also made 5 previously-hardcoded quality constants UBO-tunable (new sliders, all
  defaulting to prior fixed behavior): `N_VIEW`/`N_LIGHT` (main atmosphere loop, `cloud.viewSamples`/
  `lightSamples`) and ocean's `seaMap`/`seaMapDetail` octave counts + reflection sample count
  (`cloud.oceanSeaOctaves`/`oceanDetailOctaves`/`oceanReflSamples`). `CloudParams` grew 208→224
  bytes. **Session 29 update:** real GPU-timestamp profiling superseded the guess that `N_VIEW`/
  `N_LIGHT` (and a transmittance LUT) were the lead cost suspects — `optDepth`'s isolated cost was
  consistently near-zero; terrain's step-count formula (see below) and aurora (see its own entry)
  were the real dominant costs. See `TERRAIN_PLAN.md` session 24 log for the original follow-up,
  session 29 log for the profiling toolkit and the corrected picture, and "Subsystem: GPU
  Performance Profiling" below for how to re-run this kind of investigation.
- `SatDrawPC` is 132 bytes: `obsECEFDir (vec4)` at offset 112 (xyz = observer ECEF unit vector,
  w = obsHeightOffset), `debugDisableMask (uint)` at offset 128 (perf knockout toggles, session 29
  — see "Subsystem: GPU Performance Profiling"). `CloudMarchPC` is also 132 bytes for the same
  reason (mirrors `debugDisableMask` — only the aurora/airglow-red bits are meaningful there).
- Sky descriptor set has 17 bindings (0-16): GlowBuf, noise, moon, earthDay, earthNight, earthElev, earthSpec, earthClouds, cloudNoiseTex (sampler3D), CloudParams UBO, half-res cloud march targets A/B, lightDomeBuf, milkyWayTex, cityDayDetail, cityNightDetail, auroraNoiseTex (sampler3D, session 29)
- GPU-side observer ground height lookup added; CPU observer height also corrected (see elevation encoding below)
- `sat_sky.frag` ground path: terrain march step count is path-length-adaptive as of session 29
  (`kN` scales with this ray's own `tExit`, clamped to a user-tuned [64,164] range — the old
  altitude-only formula gave a grazing/horizon ray and a steep ray from the same observer altitude
  identical step budgets regardless of how much further the grazing ray actually travels, a real
  bug contributing to reported terrain jitter, not just under-tuned; see `TERRAIN_PLAN.md` session
  29 log) + 12-step binary search; terrain hits use gradient-computed normals; sea-level sphere
  fallback; satellites/stars depth-tested against terrain (gl_FragDepth: close terrain → [0, 0.5),
  sky → 1.0)
- Ocean wave material: specular map (binding 6) gates UBO-tunable-octave noise wave normals +
  Blinn-Phong sun glint (exp=300) + Schlick Fresnel on sea-level sphere hits
- **Volumetric clouds (C7+C8):** shell march with full C8 lighting:
  - `cloudDensity` takes two UVW args — `uvwPresence` (Z=posZ) for Perlin R threshold,
    `uvwDetail` (Z=hNorm×kVertTiles) for Worley erosion. Keeps cloud-existence horizontal only.
  - **Altitude-stratified stepping:** `stepLen = (shellThick/N) / max(abs(dir.z), 0.02)`.
    Equal altitude per step regardless of ray angle — no oblique-angle slab artifacts.
  - **Spectral sun color:** `sunColorCloud` from `optDepth` at shell entry → orange/red at sunset.
    Night-side gated by Earth shadow test. Replaces old gray `vec3(1.0)` lighting.
  - **Night darkening:** ambient transitions from blue day dome to near-zero at night using
    per-sample `dot(normalize(pECEF), sunDirECEF)` geographic terminator check.
  - **City upwelling:** `earthNightTex` at mip 3 contributes warm orange into cloud bases at night.
- **Aurora (C16):** visual design centered on the geomagnetic pole (`kGeomagPoleECEF`, antipodal
  dipole model covers both hemispheres with one constant), colatitude oval band (`auroraOvalMask`)
  with ripple-warped centerline, anisotropic curtain-fold noise (`auroraCurtainNoise` — high freq
  along azimuth for many separate folds, low freq along colatitude so each fold reads as a long
  streak, not a blob). Day-gated PER-SAMPLE on that sample's own geographic day/night (mirrors
  airglowRed's `rDayness`/`rNight`). `CloudParams` grew 288→304 bytes for `stormStrength` +
  `auroraGain` (mirrored in `cloud_march.comp`, which hand-duplicates this UBO layout, and in
  `GpuCloudParams`). `kAuroraScale = 0.000001`, same order as `kAirglowScale`. Visual
  design/tuning **DONE, closed 2026-07-16 (session 28 follow-up #22) after 22 follow-up rounds**
  — brightness/exposure, per-sample day/night gating, fold axis/unit calibration, cloud occlusion
  depth-ordering, terrain/ocean/cloud ambient lighting + ocean reflection glint, atmospheric
  extinction, light-pollution/moonlight suppression, a sigmoid-based airglow blend at both shell
  edges, per-column elevation variation, and organic domain-warped shimmer evolution. See
  `TERRAIN_PLAN.md` session 28 log (all 22 follow-ups) for the full design and bug history.
  **Architecture changed significantly in session 29 for performance** (a ~40fps-swing cost down
  to a minor one) — read this before touching aurora code:
  - The sky curtain march itself (whole-ray bounding pre-check → adaptive-step march →
    light-pollution/moonlight suppression → extinction) moved OUT of `sat_sky.frag` into
    `cloud_march.comp`'s `auroraMarchCS`, running at half resolution alongside clouds/cirrus.
    Its result folds additively into the same `B_total` channel clouds already write — no new
    sampling code needed in `sat_sky.frag`.
  - `sat_sky.frag` keeps its own copies of `auroraFrame`/`auroraCoverage`/`auroraOvalMask`/
    `auroraCurtainNoise`/`auroraSampleAt` — still used by `auroraGlowAt` (terrain/ocean ambient
    lighting) and the ocean sky-reflection's own aurora sample, both legitimately full-resolution.
    `cloud_march.comp` has near-verbatim duplicates of the same functions for its own march — keep
    both copies in sync, same standing rule as the cloud/cirrus code this file already duplicates.
  - Most of the curtain-fold and column-window noise is now baked into a texture
    (`aurora_noise.comp`, 1024×16×256 RGBA8, `createAuroraNoisePipeline` — same one-shot-bake-at-
    init pattern as `cloud_noise.comp`) instead of computed live via `warpPerlin3` every sample —
    the direct fix for "aurora is much more expensive than clouds despite looking simpler," since
    clouds' noise was already baked in a prior session and aurora's never was. New descriptor
    binding 16 (`auroraNoiseTex`, sampler3D) on `sat_sky.frag`'s set, binding 8 on
    `cloud_march.comp`'s own set (separate descriptor sets, same underlying image/sampler).
  - Real behavior change, not just perf: aurora now respects terrain occlusion the same way clouds
    do (folded into the same terrain-gated composite branch) — previously it ignored terrain
    entirely. Judged more physically correct and accepted without further tuning.
  - `kAuroraStepsMin`/`kAuroraStepsMax` are 4/64 (was 24/160) — user-tuned in-app; the min rarely
    binds (a straight-down path through the ~200km shell is already ~14 steps at the target
    resolution). See `TERRAIN_PLAN.md` session 29 log for the full four-round history (step cap →
    pre-filters → noise bake → resolution move) and the specific approximations each step accepted.
- **Next:** C14 (Anvil) remains not started — deferred repeatedly in favor of C15/C16 per the
  2026-07-12 session — and can be picked up whenever; it has no dependency on C15/C16. Otherwise
  Phase E is complete (C13, C15, C16 done); C9/C11/C12 and noise-repetition cleanup are next in
  line per the 2026-07-12 planning session's priority order. Resolution scaling shipped in session
  29 (background-only, satellites/stars/UI always native res) — see "Subsystem: Resolution
  Scaling" below.

### Elevation texture encoding — READ THIS BEFORE TOUCHING TERRAIN CODE

**File:** `assets/textures/earth_elevation.png` (R8_UNORM, 21600×10800, land-only DEM)

**This is NOT ETOPO1 and has NO bathymetry / below-sea-level data.** Do not assume
pixel=0 means sea level — it does not. The actual encoding is:

| Pixel value | Meaning |
|-------------|---------|
| 0–14/255    | Compression noise in ocean regions — treat as sea level |
| **15/255**  | **Ocean / sea level baseline** |
| 16–255/255  | Land elevation, linearly scaled above sea level |
| 255/255     | ≈ 8848 m (Everest) |

**Correct formula used in `sat_sky.frag` and `SatelliteSim.cpp`:**
```
kElevOffset = 15.0/255.0 * kElevRange          // ≈ 529 m baseline
terrainH    = max(0, pixel * kElevRange − kElevOffset)
```
Failing to subtract `kElevOffset` makes every coastline on Earth appear as a ~530 m vertical
cliff because sea-level land reads as 529 m above the ocean sphere. This bug has been
introduced and re-introduced across multiple sessions. The ocean sphere sits at exactly
`R_EARTH`; the terrain height formula must produce 0 m for ocean-baseline pixels.
