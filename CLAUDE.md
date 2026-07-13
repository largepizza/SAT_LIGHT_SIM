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

**SatFlarePC** (100 bytes) — sat_flare.comp:
```
enuX (vec4), enuY (vec4), enuZ (vec4)  — offsets 0/16/32
sunDirECI (vec3), satCount (uint)      — offset 48/60
obsECI (vec3), pad (float)             — offset 64/76
brightnessScale, daySuppression, mirrorBoost, visThresh, highlightFlare  — offsets 80–96
```

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
| `daySuppression` | 500.0 | sky background suppression ratio |
| `mirrorBoost` | 300.0 | mirror peak multiplier (MIRROR_BOOST) |
| `visThresh` | 0.0 | visibility cull threshold |
| `highlightFlare` | 0.05 | fixed flare for highlight/census mode |

`effectFlare = flare / (1 + dayBright × daySuppression)`
`magnitude = kMagRef - 2.5 × log10(effectFlare / kMagRefFlare)` where `kMagRef=6.0`, `kMagRefFlare=0.008`

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

## Fixed Simulation State

**Start epoch**: UTC 2036-06-21 00:00:00 → J2000 seconds = 1,150,891,200 (stored split: day 13,320 + 43,200 s)
**Observer**: 67°S 67°W → ECEF `obsDir = {0.1527, -0.3596, -0.9205}`, facing north
**Moon phase offset**: `kMoonPhaseOffsetRad = 3.916 rad` → originally calibrated for 2026-03-30; moon phase at new epoch will differ

---

## Active Development: Earth / Terrain Rendering

See `TERRAIN_PLAN.md` in the project root for the full step checklist and session log.
Read it at the start of any terrain-related session before making changes.

**Current state (as of 2026-07-12, session 22):**
- Steps 1, 2, 3, 4, 5, 5b, 6, 8 complete; C1–C8, C13, C15 complete
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
  atmosphere loop for free (their peaks fall inside its ~100km ceiling); red needs its own small
  16-step supplemental march out to `R_EARTH+500km` since its peak/half-width sit well past that
  ceiling — extending the primary loop's far bound for one band would have coarsened the
  near-surface Rayleigh/Mie sampling everything else depends on. `CloudParams` UBO grew 176→192
  bytes (all 3 pad slots were already consumed by C13) for 4 new gain fields (`airglowGain` master +
  per-band); peak altitude/width/color are hardcoded physical constants, not UBO fields. Reuses the
  analytic `warpPerlin3` noise (same one `cloudWarpOffset` uses) for horizontal patchiness — no new
  texture/binding. See `TERRAIN_PLAN.md` session 22 log for the full writeup.
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
- `SatDrawPC` is 128 bytes: `obsECEFDir (vec4)` at offset 112 (observer ECEF unit vector)
- Sky descriptor set has 10 bindings (0-9): GlowBuf, noise, moon, earthDay, earthNight, earthElev, earthSpec, earthClouds, cloudNoiseTex (sampler3D), CloudParams UBO
- GPU-side observer ground height lookup added; CPU observer height also corrected (see elevation encoding below)
- `sat_sky.frag` ground path: 96-step quadratic terrain march + 12-step binary search;
  terrain hits use gradient-computed normals; sea-level sphere fallback; satellites/stars
  depth-tested against terrain (gl_FragDepth: close terrain → [0, 0.5), sky → 1.0)
- Ocean wave material: specular map (binding 6) gates two-octave noise wave normals +
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
- **Next:** C16 — Aurora, geomagnetic curtain primitive (see `TERRAIN_PLAN.md` "Immediate Next
  Step"). C14 (Anvil) remains deferred — pushed back again in favor of C15 per the 2026-07-12
  session — and can be picked up whenever; it has no dependency on C15/C16. Phase E (C13–C16:
  Cirrus, Anvil, Airglow, Aurora) takes priority over C9/C11/C12 and noise-repetition cleanup per
  the 2026-07-12 planning session.

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
