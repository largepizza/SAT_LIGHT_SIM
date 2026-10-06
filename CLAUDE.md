# CLAUDE.md

This file provides guidance to Claude Code when working with this repository.
The primary simulation is **SatelliteSim** (`src/simulations/SatelliteSim.h/.cpp`).
All other simulations (GameOfLife, Particles, Scene3DDemo) are legacy and rarely touched.

---

## Index — start here

This is the internal architecture reference. It is written to be read **before** editing code: the
load-bearing invariants are stated where they apply, and many sections carry a "the first cut did X
and that was wrong" note — usually the fastest way to understand why the code is shaped as it is.
Design decisions and their dates live next to the code they constrain, not in a separate chronology.

| Document | What it is |
|---|---|
| `README.md` | User-facing: what this is, prerequisites, build, packaging |
| `wiki/` + `mkdocs.yml` | **The project wiki** (MkDocs Material): present-tense reference for using, modding, simulation, accuracy, rendering, sound, development; dated material only under `wiki/history/`. Process in `wiki/development/wiki.md` |
| `docs/CONSTELLATION_MODDING.md` | User-facing modding guide (`constellations.json`, `satellite_models/*.json`) |
| `docs/rendering/` | **As-is rendering architecture**: frame graph, cloud field / march / lighting / temporal, terrain + depth, sky composite, points + meshes; `FINDINGS.md` lists suspected bugs and doc drift |
| `docs/HARNESS.md` | **Automation harness**: scripted runs, captures, perf, UI dumps — how an agent runs and sees the app |
| `docs/FREEZES.md` | The 2026-09 machine freezes (unresolved; mitigated by launch spacing) and the forensics tools built for them |
| `CHANGELOG.md` | What changed per release |
| `data/benchmarks/KNOWN_RESIDUALS.md` | Accepted photometric error, with the measurement behind it |
| `.plans/*.md` | **Untracked local design logs** (phase plans, terrain, cloud perf, boot loader). Sections below link to them; a fresh clone has none |

**Read in this order (~10 minutes) if you are new:** Build Commands → Architecture → Frame Loop Order
→ Unified scene depth → Satellite Types (Rigid attitude groups, Geometry models) → Photometry /
Shader Constants → VulkanContext Helpers. Then jump to the section you actually need.

**Build & release** — *Build Commands* (configure/build/run, exe name, and the rule about
launching the app: only through the harness) · *Presets and release packaging* (the two Windows release paths,
`package-release`) · *macOS release architecture* · *Old / low-end hardware floor* (the
guaranteed-minimum table and the push-constant gate).

**Architecture** — *Frame Loop Order* (the canonical pass order) · *Loading screen* (frames presented while init runs) · *Unified scene depth* (one
log-distance encoding, written by every surface) · *Occlusion: one shared depth buffer* (why
`sceneDepthImg` is half-res R32F, and what it replaced) · *Shader `#include`* (the header table, the
`CloudParams` mirror and `check_cloud_params.py`).

**Satellites: data, geometry, orbits** — *Satellite Types* (catalogue, **Rigid attitude groups**,
**Geometry models** — SatModel, materials, lobes, earthshine, transmission, render-only parts,
lattices, patterns — legacy attitude modes, `typeIdx`, adding a type) · *Orbital Mechanics /
Constellations* · *GPU Orbital Pipeline* (two-dispatch pattern, buffers, `Gpu*` layouts, push
constants) · *TargetedReflector / Mirror Ground Targets* · *Reflect-Orbital Beam Cloud Occlusion* ·
*GpuSatInput* (a tombstone — the buffer was deleted 2026-09-22).

**Rendering** — *UIRenderer / Clay* (icon atlas, the fixed-bitmap font, manual hit-testing, the **UI kit** every window is built from, HUD text entry) · *Sky TAA* (the background's temporal AA, and the main pass's shared dependencies) · *The Moon as a body* (true position and size, eclipses) · *The sea* (periodic waves, sea state, shore) ·
*Photometry / Shader Constants* (lobe model, bloom and glare) · *Light Pollution Dome* · *Atmospheric
Extinction* · *Sky Glow SSBO* · *Planets* · *Cloud Shadows* · *Resolution Scaling* · *Weak-Hardware
Sky Tiers (Potato / SKY_LITE)*. The mesh renderer, model viewer and environment probes are
subsections of *Satellite Types* (Phases 4b–4f).

**Sound** — *Ambient sound* (the layer table, the context drivers, procedural voices + CC0
samples, the tonality that follows the soundtrack (`MusicAnalysis`), the harness's offline audio
mode and the mix rule).

**State, profiling, cinematics, terrain** — *Persistent Settings* · *Fixed Simulation State* · *GPU
Performance Profiling* · *Intro Cinematic (UC3)* · *First-run tutorial* · *Bookmarks* · *Controls / Keybinding Pipeline* · *Active
Development: Earth / Terrain Rendering* (read **Elevation texture encoding** before touching terrain
code).

**Tools and gates** — `tools/harness/` (the automation harness: `run.py`, `live.py`,
`imgtools.py`, `selftest.py`, `gc.py`, `tstab.py` (temporal stability) — docs/HARNESS.md) · `tools/sat_model_tool/` (SatModelTool: bake,
validate, benchmark, trace replay) · `tools/check_cloud_params.py` · `tools/parse_bsc.py` · `tools/make_icons.py` (regenerates the
UI icon PNGs from geometry declared in that file) · `tools/make_ambience.py` (the ambience samples,
from CC0 sources declared in that file) · `tools/sound_tool/` (SoundTool: soundtrack analysis +
offline synth renders) · `tools/benchmarks/` ·
`cmake/AccuracyGate.cmake` (`cmake --build build --target accuracy-gate`) ·
`cmake/PackageRelease.cmake` (the single "what ships" list).

**Wiki routine** — never edit `wiki/` pages during feature work. When a change alters something the wiki
describes (behaviour, technique, file format, setting/default, accuracy or perf number), finish by leaving a
note in `wiki/_inbox/` with the `wiki-note` skill. Pages are rewritten only in a dedicated pass (`wiki-pass`
skill), which folds the notes in as present-tense prose, files their history under `wiki/history/`, and must
pass `python tools/wiki/check.py`.

**Rules that bite (each one has a section behind it)** — launch the app only through the harness
(`tools/harness/run.py`, docs/HARNESS.md), never interactively, and never back-to-back (the harness
spaces launches 30 s apart); how a change *feels* is the user's to
judge; `GpuCloudParams` is a hand-maintained mirror of
`cloud_params.glsl` (run the checker after touching either); never store a distance in a half-float;
every push-constant struct `static_assert`s to exactly 128 bytes; `CMakePresets.json` and `.vscode/`
are committed and must stay free of absolute paths; `.plans/`, `.claude/`, `build*/`, `dist/` and
`benchmark_runs/`, `harness_runs/`, `harness_live/` are gitignored.

---


## Build Commands

```bash
cmake -B build -S .                           # configure (downloads deps via FetchContent)
cmake --build build                           # build + compile shaders + copy SPVs next to exe
cmake --build build --config Release          # release build
```

Run: `build/Debug/SAT_LIGHT_SIM_V_<version>.exe` (e.g. `build/Debug/SAT_LIGHT_SIM_V_1_2_0.exe` — the
exact name tracks `VERSION`; see `CMakeLists.txt`'s `EXE_BASENAME`).

Shaders: auto-detected glob (`shaders/*.vert|.frag|.comp`), compiled by `glslc`, copied as `shaders/*.spv`. New shader files are picked up automatically on next build.

Runtime files (`assets/`, `data/*.json`, `data/satellite_models/`, `THIRD_PARTY_NOTICES.txt`) land next
to the exe via **two** mechanisms, and the second one is the one that matters day to day: the POST_BUILD
copy steps (for a fresh build dir), and `sat_sync_runtime_sources()` — a stamp target that DEPENDS on
those files, so changing one (e.g. regenerating an icon with `tools/make_icons.py`, or editing
`constellations.json`) re-syncs the exe's directory on the next build **even when no C++ changed**.
Without it, nothing relinks, nothing copies, and the exe keeps loading the old file that is already
sitting next to it — the change just silently "does not take".

**Screenshots survive the build tree.** The app writes user screenshots to `<exe dir>/screenshots`,
and that is inside the build tree (`build-win-release/Release/screenshots` for the `windows-release`
preset) — which `release.bat` deletes outright (`rmdir /s /q build-win-release`) before every release
build, taking the images with it. `sat_ensure_screenshot_dir()` (`cmake/EnsureScreenshotDir.cmake`),
an always-run target that the exe depends on, keeps that path a link instead — a directory junction on
Windows, a symlink elsewhere — pointing at the source tree's `screenshots/` (gitignored; the published
`docs/screenshots/` images are separate). Reconfiguring recreates the link, and a pre-existing real
`screenshots/` directory is migrated into it rather than lost.

**Launch the app only through the automation harness** (`tools/harness/run.py` / `live.py`,
docs/HARNESS.md) — never interactively (no `run` skill, no bare exe). A harness run is scripted,
exits on its own, runs muted in its own user-data folder (it cannot touch the user's
`settings.json` or trigger crash recovery), and returns captures, state sidecars, layout dumps and
timings you can check. Use it to verify rendering, performance and UI layout. How a change *feels*
(camera motion, input, audio, a menu's ergonomics) is still the user's to judge. (Until 2026-09-25
this rule was "never launch the app"; the harness is the controlled exception it was missing.)

**Requirements**: Vulkan SDK + `VULKAN_SDK` env var, CMake 3.20+, MSVC C++20.

### Presets and release packaging

`CMakePresets.json` holds every per-platform configuration (`windows` / `linux` / `macos`, plus
`*-release` variants and `macos-universal-release`). **It is committed and must stay free of
absolute paths** — machine-specific values go in `CMakeUserPresets.json`, which is gitignored.

**Windows has two release preset pairs.** `windows-release` / `windows-package` use the
`Visual Studio 17 2022` generator — this is the local-dev / `release.bat` path. **CI uses
`windows-ci` / `windows-ci-package` instead: Ninja + whatever MSVC toolset the runner ships**,
set up by `ilammy/msvc-dev-cmd`. The hardcoded VS-version generator name broke the v1.1.1 tag
build outright ("could not find any instance of Visual Studio") the day GitHub started migrating
the `windows-2025` image from VS2022 to VS2026; Ninja + a dev environment is version-agnostic and
matches how the Linux/macOS jobs already build. Keep both pairs — don't delete the VS one, IDE
users depend on it. `build-win-ci/` is the Ninja binary dir (gitignored).

`.github/workflows/release.yml` also runs on pushes to `main` and PRs targeting `main` (not just
`v*.*.*` tags) — build + package all three platforms, so a compiler-portability break (MSVC
accepts narrowing conversions in braced initializers that GCC/Clang reject — this is what the
v1.1.1 Linux/macOS jobs died on) or a CI-environment break surfaces in the PR, not at tag time.
The `publish` job stays tag-gated. `CMakeLists.txt` also promotes MSVC's C4838 (narrowing in an
initializer list) to an error via `sat_strict_warnings()` so that specific class fails a local
Windows build too.
`.vscode/settings.json` and `launch.json` are committed too and are under the same rule; a
hardcoded `cmake.cmakePath` and `VULKAN_SDK` under a developer's home directory shipped there once
(2026-09, macOS-potato branch) and reached the public repo's history.

`cmake --build <dir> --target package-release` stages and archives the distributable into `dist/`.
`cmake/PackageRelease.cmake` holds the copy list — **the only copy of it**. `.github/workflows/
release.yml` and `release.bat` both drive that target rather than repeating the list, which they
previously did five times over (3 CI jobs + 2 batch branches) and had already drifted. On macOS the
same script bundles the Vulkan loader + MoltenVK into `lib/` and writes the launcher `.command`
(there is no system Vulkan on macOS; the exe's baked-in `$VULKAN_SDK` path exists only on the build
machine).

**`CMAKE_POLICY_VERSION_MINIMUM 3.5` at the top of CMakeLists.txt is load-bearing, not cosmetic.**
CMake 4.0 turned `cmake_minimum_required(VERSION <3.5)` from a deprecation warning into a hard
configure error, and two FetchContent deps still declare one (glfw 3.4 → `3.4...3.28`,
nlohmann/json v3.11.3 → `3.1...3.14`). Without it, a stock CMake 4 — Homebrew's default, and
increasingly Windows'/Linux's — cannot configure this project at all. That failure is what the
committed absolute path to a portable CMake 3.31 was working around.

### macOS release architecture

The macOS CI job builds a **universal binary** (`CMAKE_OSX_ARCHITECTURES="arm64;x86_64"`) with
`CMAKE_OSX_DEPLOYMENT_TARGET=11.0`. Both are required and they fix *different* failures:
- **Architecture.** `macos-14` runners are Apple Silicon, so a default build is arm64-only and an
  Intel Mac rejects it with **"bad CPU type in executable"** — the kernel refusing to exec a Mach-O
  with no slice for the host, before any of this app's code runs. Universal is preferred over a
  second `macos-13` Intel runner job because GitHub is retiring those images, and it ships one
  download instead of making players choose.
- **Deployment target.** Without it clang stamps the *build* machine's OS version, and a
  macOS 14-targeted binary is refused on Monterey (12.x) with "not supported on this version of
  macOS" — the error that surfaces *next* once the CPU-type one is fixed.

The workflow's `lipo -archs` step fails the build if either slice is missing from the exe **or from
the bundled dylibs** — LunarG ships universal dylibs, but that is a property of their build, not
something this workflow controls.

### Old / low-end hardware floor

The 128-byte push-constant trim (see "Subsystem: Weak-Hardware Sky Tiers") is one of several places
this project sits above a Vulkan *guaranteed minimum*. `VulkanContext::logDeviceLimits()` logs all
of them every launch and throws a named error when one is short, so a report from a machine nobody
has access to is answerable. Current margins:

| Limit | Guaranteed min | This app needs | Why |
|---|---|---|---|
| `maxPushConstantsSize` | 128 | **128** | every PC struct static_asserts to exactly 128 |
| `maxImageDimension2D` | 4096 | **14999** | `earth_elevation.png` is 14999×7500 (GCN1/Metal cap is 16384 — little headroom) |
| `maxImageDimension3D` | 256 | **1024** | `aurora_noise.comp` bakes a 1024×16×256 volume |
| `maxComputeWorkGroupInvocations` | 128 | **256** | `local_size 16×16` in cloud_march / scene_depth / flare_blur |
| `pointSizeRange[1]` | 64 (with `largePoints`) | up to 1024 | glare sprites (`glare.vert`) clamp to it — smaller on weak parts |
| `maxComputeSharedMemorySize` | 16 KB | ~5.2 KB | tile-cull lists — comfortable |
| `maxPerStageDescriptorStorageBuffers` | 4 | **11** | `sat_orbit.comp`'s set (the check said 6, for `sat_sky.frag`, long after this set passed it — corrected 2026-09-23 with the occlusion buffers); MoltenVK is the realistic place to hit it, since it maps SSBOs + UBOs + vertex buffers into Metal's 31 per-stage buffer slots |
| `maxPerStageDescriptorSampledImages` / `Samplers` | 16 | **16** | `sat_sky.frag` — AT the floor since terrain v2 P3's material array (binding 27, 2026-09-29): a new sky texture must merge into an existing one (e.g. the two city detail maps into an array) |
| `maxPerStageDescriptorStorageImages` | 4 | 2 | `sat_sky.frag`'s Phase 4c mesh targets (imageLoad) — added as storage images precisely because the sampled-image budget above had one slot left |

**The push-constant gate in `pickPhysicalDevice()` read 144 until 2026-09-08** — the pre-trim
`SatDrawPC` size — so it rejected precisely the hardware the trim was performed for. Keep that
constant equal to the largest PC struct; if one grows past 128 the fix is the CloudParams UBO, not
a bigger number there.

Two more non-guaranteed things are now checked rather than assumed: device **features** are
verified present before `vkCreateDevice` (requesting an unsupported one fails with
`VK_ERROR_FEATURE_NOT_PRESENT` and no indication of which), and **linear blit filtering** is a
per-format optional feature, so `VulkanContext::bestBlitFilter()` picks LINEAR/NEAREST per format
for both mipmap generation and the `renderScale < 1.0` upscale — the latter being the path that
exists *for* weak hardware, the least safe place to assume the optional feature.

VRAM is the untested one: the shipped textures are roughly 500 MB of GPU memory before mips
(8K day/night/clouds/specular/Milky Way + the 14999×7500 R8 DEM ≈ 112 MB on its own), against
2 GB on a 2015 MacBook Pro R9 M370X. Nothing streams or downsamples them. **Until 2026-09-22 the
per-satellite buffers reserved another ~2.2 GB on their own** (orbit + the since-deleted
GpuSatInput + visible, all allocated at `MAX_SATELLITES` = 10M); `createSatBuffers()` now sizes
them to the loaded roster (~100 B/satellite, ~131 MB for the default ~1.38M, ~940 MB at 10M).

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
                           recordMeshScene() — Phase 4c mesh instances (CPU double poses, camera-
                                               relative) + env-probe cube faces. Runs BEFORE
                                               scene_depth.comp, so clouds and beams clamp to a
                                               mesh (two mesh draws: the depth pre-pass, then the
                                               scene pass with depth EQUAL; then, for mirror pixels,
                                               the SKY_REFL sharp-reflection pass + mesh_refl_add)
                           dispatch 1: scene_depth.comp   (half-res shared terrain/ocean/MESH depth —
                                                            the detailed terrain march, terrain_detail.glsl;
                                                            also writes the observer's ground height into
                                                            terrainFrameBuf for sat_sky.frag; then the
                                                            harness probe, if one was requested)
                           dispatch 2: sat_orbit.comp     (orbital mechanics + attitude + beam list +
                                                            reflectance model → APPENDS visible
                                                            satellites to the compact satVisibleBuf
                                                            list + indirect args in satListBuf)
                           dispatch 3: beam_self_march.comp (per-beam cloud occlusion, up to 2048
                                                            beams — replaced beam_cloud_block.comp's
                                                            201-target version 2026-08-09, see
                                                            "Subsystem: Reflect-Orbital Beam Cloud
                                                            Occlusion" below)
                           dispatch 4: cloud_march.comp   (half-res clouds/cirrus/aurora/airglow-red/
                                                            beam pointing ray + volumetric glow +
                                                            per-pixel cloud shadow)
                           dispatch 5: sat_flare.comp     (INDIRECT; photometry in place on the
                                                            compact list: sky/extinction/pollution +
                                                            visibility culling + sprite size)
                           flare-source pass (graphics, its own render pass): satellite sprites +
                           the sun's virtual point, then mesh_bloom.frag's mesh over-white energy
                           dispatch 6: glare_find.comp     (only when meshes drew this frame: 5×5
                                                           local maxima past the threshold into
                                                           glintBuf, drawn as glare_mesh.* later)
                           dispatch 7: flare_blur.comp     (ping-pong gaussian/streak: source ↔ scratch)
                           trail pass (when enabled): trail_fade.comp, then the satellite/star/planet
                           splat draws into trailAccumImg
                           recordModelViewer() — Phase 4b model-viewer window (offscreen, own pass)
                           barriers between each (see recordCompute for exact stage/access pairs)
sim->recordPrePass(cmd)  → renderScale < 1.0 only: low-res sky → vkCmdBlitImage into swapchain
vkCmdBeginRenderPass     → owned by App
sim->recordDraw(cmd)     → sky/ground background (the mesh is a surface in it, Phase 4c) →
                           satellite points → stars → planets → flare composite → glare
                           (satellite sprites + mesh glints) → trail composite
ui.record(cmd)           → Clay → Vulkan quads/text/icons on top
vkCmdEndRenderPass       → owned by App
sim->recordScreenshotCopy() → UC6 screenshot blit after the pass — no-op unless a shot is pending
```

### Unified scene depth (Phase 4, 2026-09-23)

The main render pass's depth attachment holds ONE encoding, written by everyone:
`log2(t / 1 cm) / log2(1e9 m / 1 cm)` of the **true distance** along the view ray
(`shaders/include/depth.glsl`, `sceneDepthFromDistance()`), compare LESS, cleared to 1.0. The sky
passes (`sat_sky.frag`, `_lite`, `_minimal`) write it for the first opaque surface — terrain, else
ocean, else ≥90%-opaque cloud — at any range, 1.0 for sky; satellite points write their own range
(`GpuSatVisible::rangeM`); stars and planets draw at `kDepthFar` (infinity, behind every surface).
Phase 4's meshes join the same encoding. The manual tests (trails, `renderScale < 1`) and
`flare_source.frag` apply the same rule against `sceneDepthImg`: a surface occludes only if nearer
than the object.

**It replaced a 150 km cap** (`kOcclusionCap`): terrain wrote `t / 300 km` only within 150 km and
points sat at a fixed 0.5 — so that from orbit the distant Earth didn't swallow the satellites in
front of it. Consequences of the cap that are now fixed: a mountain or opaque cloud more than 150 km
away never hid a star or satellite, and `flare_source.frag` (which used "any surface occludes")
killed the bloom of every satellite seen in front of the Earth from orbit. Resolution: D32 float
over 36.5 octaves ≈ 1.5e-6 of the distance, anywhere.

### Occlusion: one shared depth buffer

`scene_depth.comp` writes `sceneDepthImg` — half `ctx.swapExtent`, **`R32_SFLOAT`**, linear metres
along each view ray to the first terrain/ocean surface, or `kNoSurfaceT` (1e30) for sky. Every
later pass tests against it instead of deriving its own answer.

This replaced three separate mechanisms that existed only because `cloud_march.comp` had no
terrain data: `beamTerrainVisibility()` (an 8-32 sample DEM march run *per beam per pixel* — the
single most expensive thing in that shader), `tEnterCombined` (one entry distance fused across
cirrus+cloud+beam+aurora, so none could be occluded independently), and the opacity-gated
`tCloudOcclude` for terrain purposes. Each volumetric march now clamps to `tScene` at march time,
which also gives real partial truncation where a ridge pokes into a shell.

**Sizing is deliberate and load-bearing:** half of the (render-scaled, since 2026-10-04 —
`computeHalfExtent()`) swap extent, exactly matching `cloudMarchTargetA/B`. That is what lets `cloud_march.comp` read it 1:1 with
`texelFetch` at its own `gl_GlobalInvocationID` — no UV math to get wrong. Fragment consumers use
`gl_FragCoord.xy / pc.screenSizePx`. **Consequence:** at `renderScale < 1.0` the depth pass does
not shrink, so its relative cost rises sharply (at 50% it marches terrain at the same pixel count
as the sky pass itself). Knockout bit 1024 disables it — the buffer fills with `kNoSurfaceT`, which
reproduces pre-unification occlusion behaviour, making the whole architecture one A/B checkbox.

**Never store a distance in a half-float.** `tEnterCombined` lived in an `RGBA16F` alpha, whose
65504 ceiling every near-horizon cloud entry overflowed to `+inf` — silently suppressing the entire
composite on any ray that also hit the sea-level sphere. That bug is why `sceneDepthImg` is R32.

### Shader `#include`

`shaders/include/*.glsl`, compiled with `glslc -I`. CMake globs the headers and makes every shader
depend on all of them — coarse, but `add_custom_command(DEPFILE)` needs CMake 3.27 for VS
generators and this project requires 3.20.

| Header | Holds |
|---|---|
| `common.glsl` | PI, R_EARTH/R_ATMOS, BETA_R/M, H_R/H_M, G_MIE, SUN_INTENSITY, cloud noise freqs, `kNoSurfaceT`, `raySphere`, `rotateZ`, `remap`, phase functions |
| `terrain.glsl` | DEM decode constants, `dirToUV`/`posToUV`, `terrainHeightAtUV/AtDir`, `enuBasis`, `observerEffHeight`, `observerPos` |
| `cloud_params.glsl` | the `CloudParams` UBO block + `CloudLayer` (`#define CLOUD_PARAMS_BINDING` first) |
| `terrain_detail.glsl` | procedural terrain detail + THE terrain march (needs common, cloud_params, terrain first) — see "Procedural terrain detail" |

**`GpuCloudParams` in `SatelliteSim.h` is a hand-maintained mirror of `cloud_params.glsl`** — GLSL
and C++ cannot share a declaration, so that pairing is the one place a CloudParams mismatch can
still hide. **Run `python tools/check_cloud_params.py` after touching either.**

The failure mode is a *permutation*, not a size change, which is why the `static_assert` does not
catch it: append a field in a different position in each file and the total size is unchanged, so
everything compiles and every field from the divergence point onward silently reads its
neighbour's value. This shipped once — `flatSunGainScale` read a pad (0, so 2D clouds rendered
black) while `flatCoverageScale` read 4.0 (so coverage quadrupled and swallowed the Earth).
Prefer appending at the end of both files.
| `reflect_beam.glsl` | `ReflectBeam` + `BEAM_MAX_ACTIVE` |
| `cv2_optics.glsl` | the bows, halos, pillar and `cv2HG` (pure functions): the cloud march and the rain particles share them |
| `rain_particles.glsl` | the rain particles' push constants (`RainDrawPC`) |

**`observerEffHeight` must be used by every pass that produces or consumes a distance.**
`cloud_march.comp` previously took a CPU-computed `obsEffH` while `sat_sky.frag` did its own GPU
lookup; harmless while each only compared against itself, wrong the moment one produces a depth
buffer the other reads.

**`optDepth` is the cautionary tale — and NOT in the direction you would expect.** Its two copies
look like classic drift: `cloud_march.comp` hardcodes `N_LIGHT=12` while `sat_sky.frag` reads
`cloud.lightSamples`, so the "Light samples" slider affects atmosphere and not clouds/aurora.
Sharing them was tried and **reverted after a measured performance regression**.

The reason is that de-duplication and specialization are in tension here. `cloud_march.comp`'s
trip count is a compile-time constant, so its loop unrolls; a shared function that must also serve
a settings-tunable count necessarily takes the count as a parameter, making the bound runtime.
glslc emits it as a real function (SPIR-V confirmed non-inlined, 5 call sites in that shader), so
the driver has to both inline and prove the constant to recover the unroll. It did not recover it.

**The false assumption was that extracting byte-identical code into a header is codegen-neutral.**
It is for pure declarations (`CloudParams`, `ReflectBeam` — no risk, real value, keep doing that)
and for code that was already a function with an unchanged signature. It is NOT when the signature
change turns a constant into a parameter. If you unify these anyway, measure `cloud_march` before
and after, at altitude and near the surface — do not assume.

**Still hand-duplicated, by choice:** `optDepth` (see above — sharing it was measurably slower),
and the aurora function set (`auroraFrame`, `auroraCoverage`,
`auroraOvalMask`, `auroraCurtainNoise`, `auroraSampleAt`/`auroraCurtainSample`) in `sat_sky.frag`
and `cloud_march.comp`. Verified byte-identical (comments stripped) as of this pass, so there is no
active bug — but they bind `auroraNoiseTex` at different indices and read different PC structs, so
sharing them needs sampler parameters threaded through all five. Keep both copies in sync until
someone does that work. Same applies to the cloud-column sample body, which appears in
`cloud_march.comp` (view march + sun cone + terrain shadow) and `beam_self_march.comp` (per-beam
cloud occlusion march — see "Subsystem: Reflect-Orbital Beam Cloud Occlusion" below).

---

## Subsystem: Clouds v2 — the volumetric clouds (2026-09-27; v1 deleted the same day)

Design log and status: `.plans/CLOUDS_V2_PLAN.md`. The only volumetric cloud renderer: v1's
`cloudMarchCS`, its ground shadow, its copy in `beam_self_march.comp` and its sliders are gone (there
is no toggle). Every tunable is a Clouds-tab slider AND a settings.json key under `clouds_v2`, so the
harness can sweep it (`set clouds_v2.<key>`). Cirrus (`cirrusMarchCS`) and the flat 2D layers are
still the older model (v1's ground fog was retired 2026-09-29 for the v2 fog + dust, below) and keep their own sliders ("Cirrus", "Flat layers" sections) — the old cirrus
only as the stand-in when the volumetric march is knocked out (see HIGH LAYER below).
- **One field** (`shaders/include/clouds_v2.glsl`, `cv2Field`) serves the view march, its light march,
  the ground shadow (`cloudGroundShadowV2`, cloud_march.comp) and beam occlusion (beam_self_march.comp).
  Weather cube (baked from the 8K map: coverage, classified type, precipitation, tropopause) x
  mesoscale fields on the sea-level sphere x 3D shape noise x detail erosion; 5 cloud types
  (`cv2Types`, SatelliteSim.h). Metric coordinates anchored at the observer's sea-level point (CPU
  double), like terrain detail — never an absolute ECEF position in float.
- **The map drifts** (~16 deg of longitude per 12 h), and every consumer reads ONE phase:
  `SatelliteSim::cloudDriftPhase()` = rate x (t - 2036-06-21) + `cloudDriftPhaseOffset` (session state;
  the default keeps the default rate's map exactly where rate x (t - J2000) put it). It used to be rate x
  the time since J2000 (~7500 rad), so any change of the rate threw the map to an unrelated longitude —
  and the intro FORCED its own rate (6.55e-6), which settings.json then saved: every intro silently moved
  the player's clouds. The intro now sets only the session offset. **The whole field is read in the
  drifted frame** (`cv2Drift`: the weather cube AND the noise volumes, anchors rotated on the CPU), so the
  clouds move with the map as one body, and the resolve reprojects that motion exactly (`extra.zw` = the
  drift since last frame, `obsDelta` = Rz(d) eye - prevEye + the shape wind's shift); `motion.x` is the
  volumes' relative slide it cannot follow, which raises the new-sample weight. A `time sun` jump moves
  the clouds — pick benchmark spots at a fixed time: `python tools/harness/find_cloud_spots.py <ISO time>`
  (assumes the default rate; a harness run from a user settings file should `set clouds.drift_rate
  0.0000065`).
- **Coverage** is the map's brightness remapped (`cover.xy`: clear below 0.12, overcast above 0.6);
  the raw value used as a fraction never let anywhere close over.
- **The shape is a 3D margin, not a heightfield** (2026-09-27): cloud exists where
  `m = e(x,y) + lobes(x,y,z) - g(z) - gBase(h) > 0`. `e` = how far inside the thresholded 2D field;
  `lobes` = inverted-Worley 1.75 km / 875 m / 440 m octaves in the SAME units as `e`, so they move the
  side surface sideways (cauliflower lobes, overhangs), faded with the footprint and damped near the
  base; `g(z)` = the strength needed to reach height z (convective z^1.2: small cells low mounds, cores
  towers; decks flat with a lumpy top, `alt.z` = flatness); `gBase` curls the base up toward the edge.
  **Why:** v1 and the first v2 cuts extruded a 2D presence under a height, and a heightfield cannot
  bulge: every cloud, down to the smallest puff, was a flat-bottomed can with vertical sides (the
  "cola can"). Noise on the TOP height moves a steep wall almost not at all — it has to be in `e`'s
  units. Deep types also read the cells two mips coarser (wider towers). Erosion is reduced deep inside
  (`form.z`) — full-strength erosion holed the interior and the sky showed from inside a cloud.
- **Decks** (2026-09-28): a deck's thickness follows the closed cells and a km-scale Perlin field
  (`deckVar`, `zTop`), a thicker part hangs lower, and every base is bumped in 3D by the lobes (`hbE`,
  `baseAmp` = 40 m cumulus .. 220 m stratus x `storm.w` "Base roughness"): from below an overcast is seen
  by the light it transmits, so its thickness is its texture. `CV2Field.deck/topH` let the march shadow
  a deck lit at a grazing angle by the path to its top (tens of km, far past the light march) — without
  it the terminator band glowed red from orbit.
- **Deep convection**: lobes and erosion storm.x ("Storm feature size", 2x) larger via second anchors
  (`anchorStorm`, `anchorStormDetail` — never scale an anchored coordinate: frac(a)/k pops when the
  observer crosses a period), erosion kept storm.y; **anvils** (`cv2AnvilSigma`, storm.z "Anvils"): an
  ice shield under the tropopause where the weather cube read ~30 km coarse and 25 km upwind says deep
  convection, 3 km thick at the core, flat domed lid, frayed outline; the Cb towers overshoot it by
  250 m. Straighter-walled deep towers (0.9 z^1.8) were tried and extruded vertical flutes: reverted.
- **Cells** (`cloud_v2_noise.comp` mode 2): three sizes of blobs (8/16/32 per period) in warped space,
  merged by a SOFT UNION and clumped by Perlin — max() of one or two sizes thresholded into Voronoi
  polygons from altitude.
- **Genera so far:** the low system's 5-type table (stratus .. cumulonimbus); nimbostratus = a
  stratiform deck thickened by up to 3.5 km where the map rains; a MID LAYER (`cv2MidSigma`, 2.8-7 km,
  `misc.w`, "Mid layer (Ac/As)"): altocumulus cloudlets in a thin lens where the low cloud is broken,
  translucent altostratus where it is stratiform, only where the map has cloud and a mid-level regime
  field (cluster Perlin) allows. **HIGH LAYER** (`cv2HighSigma`, 2026-09-28, `high`/`high2`, sliders
  "High layer (Ci/Cs/Cc)", density, "Cirrus stretch", "Cirrus wind"): at ~0.74 of the tropopause,
  cirrus streaks (the shape volume read with the along-wind coordinate = drifted longitude x R at a
  period that divides the equator an integer number of times — no antimeridian seam — stretched,
  meandered, sheared with height for fall streaks, combed by a 9x read), cirrostratus veils where the
  map is stratiform, cirrocumulus patches. Its samples are lit cheaply (`CV2Field.thin`: no light
  march, no ambient probe — it spans the whole sky). It replaces `cirrusMarchCS` and flat layer 1:
  `layers[1].enabled` is 0 while it is on and the march isn't knocked out. **The baked Perlin fBm is
  0.50 +- 0.057** (measured by replicating the bake in numpy): thresholds on it must be set against
  that spread — the first cut of the high layer used 0.5-0.7 and drew nothing.
  `cv2Field` = low + mid + anvil + high, so every consumer (march, light march, ground shadow, beams)
  sees them all.
- **Terrain-relative bases** (2026-09-28): the weather cube's ALPHA is the DEM height / 8 km, smoothed per mip
  (`cloud_v2_weather.comp` binding 3), read at the EARTH-FIXED direction (`cv2Ground`, the map drifts, the
  terrain does not); the tropopause it used to hold is a function of latitude (`cv2Tropo`). Low bases rise
  by 0.6 (decks) .. 0.9 (convective) x the ground, tops with them but capped at the Cb top; the mid layer by
  0.8x. Until then every base was above SEA level and the Andes/Tibet stood inside the cloud.
- **Erosion detail fades with distance toward its MEAN, at full strength** (`df = mix(0.45, fetched,
  detailAmt)`). It used to fade the erosion toward NONE and then switch to the mean where detailAmt hit 0: a
  band of uneroded cloud just inside 4 x `detail_lod_start_m`, seen from orbit as a ring (~220 km out).
- **The source map is a JPEG**: its 8x8 blocks (~40 km) sit just above/below the clear threshold. Anything
  switched on by `cov > 0` alone draws them as straight-edged rectangles (the mid layer did; it now fades in
  with `smoothstep(0, 0.35, cov)`). Scale new layers by coverage, never gate them on it.
- **High layer from orbit**: its regime follows the map's coverage over ~150 km (weather mip 4) — cirrus
  lives with the weather systems — and its streaks come in bundles (the volume read 4x coarser; the CPU keeps
  the along-wind period count a multiple of 4). On noise alone it covered the globe in even splotches.
- **Debug view 7**: per-layer optical depth (red low incl. rain, green mid, blue anvil, white high) — the
  first thing to look at when a cloud artifact has an unknown owner.
- **Motion** (`cloud_v2_resolve.comp`): the new-sample weight and the clamp box follow PARALLAX (the
  reprojection at the cloud's depth vs at infinity) plus the clouds' own evolution — not total pixel motion.
  A rotation reprojects exactly; raising the weight during a pan swapped the accumulated image for the
  quarter grid's noisy samples (a knitted cross-hatch). In parallax all four pixels of a 2x2 block take
  nearly the same update. Check motion with `path play record=` (a settled `capture` hides it).
- **Far field (orbit)**: a fractal presence from the cluster Perlin at 1x and 4x (3-90 km, `fz`), thresholded
  to the map's coverage (area ~ cov), replaces the sub-pixel cells' uniform haze as the cells stop resolving
  (`farK`), and clusters the near field a little (0.08) so orbit and ground agree on where the holes are.
  Before it, clouds from space were the map's warped 5-10 km blobs.
- **Beams as LIGHT** (2026-09-28, `.plans/BEAMS_V2_PLAN.md`, `cv2.beam`: "Beam shafts" x, 0 = the old drawn
  line; "Beam haze / dust" y): `beamLightCloud` lights cloud samples like the Moon does (3-step march toward
  the satellite, the cloud phase lobes, ice/rain optics); `beamShafts` integrates each culled beam's Gaussian
  column across the view ray in closed form (air + aerosol scattering at the closest point, phase at the
  angle to the beam, occluded by the ray's clouds through transmittance checkpoints `tq`) inside the march,
  so the resolve anti-aliases it. Verified in pass 10 (harness_runs/cloud_v2_p10b, _p10c: Anchorage, 133-214 beams, clear and cloudy). **Pass 10 (2026-09-28):** a beam is a DISK (`beamDisk`: the Sun's
  limb-darkened disk seen in the mirror, radius = `footprintRadM`, edge blurred over ~the mirror's width),
  not a Gaussian of that sigma cut at 4 sigma — that lit ~16x the area, drew a glow ~4x the spot ending in
  a hard circle ("blobby, sharp-edged beams"), and crowded ~4x the beams into each tile (`kBeamDiskCut`
  1.1 in the cull). The ground spot (sat_sky.frag, same total energy) follows. Light is PHYSICAL: mean
  irradiance = reflecting area / disk area in Suns (`beam.w` = 1/(1361 beamGain) undoes sat_orbit.comp's
  intensity), x `beam.z` "Beam light on cloud" (30: the night view's moon is ~2400x physical, so 1 reads
  dim beside it); v1's `beamSkyGlowGain` no longer applies. `beamLightCloud` sums the lights first and
  marches occlusion ONCE along their mean direction: three field samples per beam per cloud sample cost
  350 ms frames over Anchorage (the user's profile; 3-4 ms after). Harness framing: `beams` / `beams go`
  (the site where beams converge NOW — a guessed time and site framed none three times), coverage forced to
  0 / up for clear / cloudy nights (harness_runs/cloud_v2_p10b.satcmd).
- **Rain / sleet / snow / diamond dust at the eye are PARTICLES** (2026-10-04, `rain_particles.vert/.frag`,
  `SatelliteSimRain.cpp`): one instanced draw in the main pass after everything else, i.e. AFTER the sky TAA,
  at full resolution, premultiplied over the tonemapped frame (ONE / ONE_MINUS_SRC_ALPHA: a drop dimmer than the
  sky behind it darkens it). Everything is procedural from `gl_InstanceIndex`: nested world-fixed boxes in the
  rain frame (`cv2.rainE/N/U`, the nearest 0.25-degree point, eye position from double wrapped to 1024 m), box k
  = 4 m x 2^k holding N0 x 2^k drops ("Drop particles (x1000, nearest box)" `rain_particles_k` 16, slot 261),
  drawn over [L/4, L/2] and cross-faded with the next over [0.4, 0.5] L, out to "Drop reach (m)" (`drop_reach_m`
  32, slot 208, at most 64 = 6 levels; the old `drop_distance_m` was the lattice's and is not read). Rain drops
  take a Marshall-Palmer size (0.5-5 mm) and its terminal velocity, FIXED per drop (an offset is v x t with t up
  to 600 s); flakes 2-8 mm at ~1 m/s, fluttering. **Motion is integrated over sim time on the CPU**
  (`updateRainMotion`, 2026-10-05; `cv2.rainMotion` / `rainWind`, wrapped to 1024 m): the fall phase (a drop's
  offset = its terminal speed, quantised to 1/64 m/s, x the phase) and the wind drift of rain and of snow, so speed
  and wind change smoothly (the first cut used v x t with t up to 600 s: both had to be constants). Fall = "Rain
  fall speed (x)" (`rain_fall_speed` 1.3, slot 263) x 1.35 at rain rate 0.6+. Wind = "Rain wind (x ground wind)"
  (`rain_wind_gain` 1.5, slot 264) x 0.4 x the wind aloft, toward the east, + a shower's OUTFLOW "Storm wind (m/s)"
  (`rain_storm_wind_mps` 14, slot 265, full at rate 0.6) away from the rain map's rain-weighted centre, in gusts
  ("Wind gusts" `rain_gusts` 1, slot 266: ~4 s noise, +-20-60%, +-15 deg). The user's storm snapshot (profile_log
  43): 19 m/s. Which drops exist follows the rain map at each drop (`cv2RainMapAt` x
  `cv2RainBurst`, now in cloud_lightning.glsl): a shaft's drops appear in the far levels first. **Coverage is
  conserved:** a drop is the streak it sweeps over "Drop shutter (ms)" (`rain_shutter_ms` 33, slot 262; + the
  eye's velocity, capped at 30 m/s), each pixel's alpha the share of the exposure it spent there, and each
  particle carries (the real rain's geometric cross-section per m^3, Marshall-Palmer at R = 60 rate^1.5 mm/h;
  snow x4) / (particles per m^3): summed along a ray the particles' coverage is the rain's optical depth. Where
  one particle would carry so much that it draws as a bright dot (far, heavy rain: alpha 0.12-0.35) it fades and
  the volume carries that distance. "Drops at the eye (visibility)" (`rain_streaks`, slot 157) multiplies the
  alpha: 1 = physical. **Light = the march's rain sample at the eye**: cloud_v2_lightning.comp thread 1 writes
  `cv2RainKey` (Sun x its cloud transmittance x the Earth's shadow, or the Moon), `cv2RainKeyDir` (w the liquid
  share), `cv2RainAmb` (zenith sky through the column above as the march's ambVis, + the ground bounce + city
  light at night), already / the white balance (`kCv2RainLightOffset`); copies of the march's `sunColorAt` /
  `skyZenithAt` there (keep in step). The phase is the volume's own (`include/cv2_optics.glsl`, moved out of
  clouds_v2.glsl unchanged — the march's SPIR-V is identical): drops 42 degrees from the antisolar point flash
  the bow's colours, drops toward the Sun glow in its diffraction lobe; flakes add a fluttering plate glint;
  diamond dust (ice fog at the eye in sunshine, no rain) draws only tumbling plates' glints. The host draws
  only when the lightning pass ran this frame and the previous frame's map held rain (or diamond dust).
  Harness `state` -> `clouds_v2.rain` (mode, instances, rate at the eye / max, `max_at_en_m`, key, ambient);
  benchmark: the user's Venezuela snapshots (`profile_log` 40-41, harness_runs/rain_rebuild/base.satcmd; walk
  into a shaft with time PAUSED: the clouds drift ~40 m/s and outran a timed walk). Cost ~0.35 ms at the
  defaults (496k instances, mostly culled in the vertex shader), +0.45 at 64 m; the cloud march lost ~1 ms
  without the old drops. Not modelled: the volume still marches the rain within the particles' reach (tau
  ~0.02-0.05, a slight double count); lightning does not light the drops. **Precipitation type**:
  `cv2.precip.x` = the air temperature at the eye (`SatelliteSimCloudsV2.cpp`: -18 + 45 cos(lat)^1.5 at sea
  level, + the season swing, - 6.5 C/km); snow below ~-1 C, rain above ~2.5 C, sleet between. Only the eye's
  precipitation changes type. The `rain` ambience driver takes the liquid share (`cv2EyeTempC`) and the rain
  map's centre. **Replaced** (review 3-22): `cloud_march.comp`'s `rainDrops`, a per-pixel search of a 3D
  lattice's nearest cells in up to 9 depth layers inside the HALF-RES composite: a few hundred fat streaks, smeared
  by the sky TAA, lit by the cloud colour along the ray, a far drop "a clump" ((rad/w)^0.25) — and before it
  streaks in direction space that travelled with the eye. Debug view 6 now shows the rain at the eye (r) and in
  the map (g). **Snow shafts
  show no rainbow** (review 6: the shafts' air is 500 m above the GROUND under the eye, and the season
  swing is 0.25 x |lat|, at most 15 C — 0.35 x |lat| made the Antarctic plateau rain in November) (cloud_v2_march.comp, per ray): below freezing ~500 m up (the eye's temperature,
  6.5 C/km), the rain phase's bows give way to faint ice optics (sundogs, a weak 22 degree halo, a pillar
  under a low Sun) — a snow shower's big aggregates scatter broadly; only its few plates make optics.
- **Per-layer scales (pass 10):** the shape period ("Shape period (m) - low cloud") no longer sets the
  cirrus fibres (`cv2CirrusPeriodM`, "Cirrus period") or the altocumulus (`anchorMid`, "Mid layer
  period"); the mid layer has its own density (`atmo.z`) and the anvils only "Anvils" (neither reads
  "Density" any more). "Storm feature size" is still a multiple of the low shape period; the rain
  curtains still read it.
- **Anvils are fed by their towers (pass 10):** the weather at ~10 km (mip 1) at the point and 12/25/40 km
  upwind, Cb-typed and covered; it read one 30 km-blurred texel 25 km upwind, so only storm regions hundreds
  of km across had one and it did not sit on its towers.
- **Cloud sunlight vs the sky (pass 10):** the key light's Rayleigh is its own (`atmo.x` "Cloud sunlight
  Rayleigh", x physical), not the sky's `atmosRayleighGain`; "Twilight sky light" (`atmo.y`) boosts the sky
  ambient from ~7 deg below a sample's horizon to ~14 deg above it (the zenith alone under-counts the dome;
  past the terminator its ozone blue turns the last red light purple). Ice is DELTA-SCALED: the forward
  peak (0.45) is not extinction (`sigma x (1 - 0.45 thin)`, phase = the broad lobe, optics / 0.55) —
  full extinction dimmed the clouds under cirrus as if the peak were lost (dark bands from orbit) — and
  the high layer is lit from below by the cloud under it (albedo from the map's coverage).
- **Sky light through cloud (pass 10b):** the ambient at a sample is the DIFFUSE transmission of the cloud
  above it, 1 / (1 + 0.11 tau) (two-stream, g ~0.85; tau from the probe 250 m up or the column to the top),
  plus light from below (the ground and horizon sky, 0.35 x (1 - hf)). It was exp(-sigma x 250 m): ~0 inside
  cumulus, so every large cloud's lower part was one flat dark grey under a hard line (user report, near sunset).
- **Beam shafts are a ray-cylinder intersection (pass 10b):** the stretch of the view ray inside each beam's
  column (clipped to the eye, the scene depth, the ground point and 60 km), with the air integrated along it;
  the column's edge widens to 1.5 pixel footprints (energy kept). The closest-point chord it replaced turned
  with the camera when standing inside a beam (billboarding). The light cull reaches 60 km, not the cloud top.
  Debug view 8: r the tile's lights, g the beams a ray's interval found, b the shaft radiance (log).
- **Pass 11 (2026-09-28):** the user's cloud settings are the compiled defaults (clouds_v2 only; the presets' 
  max_iters/light_steps/step_growth and the perf-heavy full_rate_above_km 0 were left out). **Flow** (anchorFlow, 
  flow.x, sliders "Flow warp" / "Flow scale"): the mesoscale reads, and half as much the weather lookup, are 
  displaced by a smooth 2-component field (the Perlin channel at 6000 km, coarse mip) so systems bend into curves; 
  the shear must stay < 1 (0.07 of a 2400 km period folded the low field into combed brush strokes). **Layer 
  spread** (flow.y): the mid layer keys on the weather over ~80 km (mip 4), not only the low cloud under it, and 
  cirrus less on the low systems. **Storm veins**: the erosion detail volume uses a smooth-min F1 (no creases) and 
  deep convection bites 70% fewer cracks. **Beam lines** (atmo.w, 0.25) draw each beam's own streak beside the 
  shafts, Rayleigh-blue and 1-20 km; shafts write their weighted distance as depth so the resolve reprojects them 
  at the right parallax while walking.
- **Pass 12 (2026-09-28):** **Convective profile** (`cv2FieldLow`, "Top-heavy" `flow.z`, "Tower top" `flow.w`,
  0 = the pass-11 cones for A/B): each CELL is one cloud — its top T from the cell-scale strength (the cells
  2 mips coarser, 4 for deep types) at most "Tower top" of the span, and a required strength parabolic about
  its widest point high up (a narrower stem, a round dome; no flat stretch, which extruded the 2D field into
  vertical walls), capped by the cells' own dome (smooth min) so dense fields keep turrets instead of mesas;
  convective lobes 2.5x, faded in from the cell edge. It was `e - z^1.2` with each column's own top: a cone
  per cell, and strong cores cut flat at the type's top. Tried and dropped: normalising e by the cell's peak
  (dense fields became slabs). Weak cells now have a minimum height (0.25 of the span): fair-weather fields
  draw as fields of puffs where the old shape drew a few dots. **Flow**: the early out read the map at the
  UNflowed point at mip 4 while the lookup moved up to ~200 km, cutting clouds along 80 km texel edges; it now
  reads the flowed point (`cv2FlowDisp`, fixed mip), and the mid/high/anvil layers read the same flowed map
  (`cv2FlowWeatherDirAt`). **Beam cloud light**: only beams landing within ~1 footprint of their site (the
  line's perpendicular miss) feed its light, fading out as they leave; slewing beams get none (a departing
  mirror dragged the site's light out, then it snapped back).
- **Pass 13 (2026-09-28):** **The flow is ANALYTIC** (`cv2FlowDisp`: the curl of eight smooth waves on the
  drifted sphere, float, divergence-free; `anchorFlow` = x its slow evolution phase, w 1/period). It was the
  RGBA8 mesoscale volume's Perlin channel at a coarse mip, and hardware filtering has 8-bit sub-texel weights:
  at ~375 km texels the displacement stepped every ~1.5 km, shifting the clouds by hundreds of metres per step
  — blocks with straight seams on the texture grid, anchored at the observer, worse with the warp (and the
  v1 distortion had the same). Never read a displacement (or anything multiplied up by a large factor) from a
  hardware-filtered 8-bit texture; the DEM hit the same limit. **Cirrus** (section "Cirrus (high layer)"; the
  old "Cirrus" section is v1, drawn only with the march knocked out, now labelled legacy): its regime noise is
  read at its own field size (`high2.y`, "Cirrus field size", 1200 km, two octaves) on a frame displaced by the
  low flow x `high2.z` ("Cirrus flow", 1.5); it was the cluster-scale noise (splotches) and ignored the flow.
  **Cb columns**: strong deep cores (`deepCol`) run the full span up to the anvil (capped at "Tower top" the
  anvil floated free) with a concave profile — wide base, a waist (0.55 + 0.2 x Top-heavy), a flare under the
  lid, flat top. Every deep cell at full height cost 56 ms inside a storm; cores only, ~the old cost.
  **Snapshots carry the view**: `view` (obs_dir, height, camera, cloud drift phase/rate) and the full
  `settings`; harness `snapshot <profile_log.jsonl> [index=]` restores one exactly — the user's way to hand
  over a location (lat/lon + time alone framed other clouds: the drift phase is session state).
- **Pass 14 (2026-09-28, the user's snapshots):** **The weather-lookup warp folded the map**: its third
  component was the closed-cell RIM field (`ce.b`, a near step) x 7 km, so the lookup jumped ~4 km across
  every Voronoi rim and storms extruded the folds into planar walls and fins; now smooth Perlin only. The
  weather cube (`cv2WeatherSmooth`) and the coarse cell reads (`cv2MesoSmoothG`) are read C1-smooth (the
  fractional texel coordinate smoothstep-remapped before one fetch): linear filtering of 5 km texels / a
  4-16 texel mip is flat facets with straight creases. Deep cells use the coarse cells only (the fine
  cells' edges were grooves up a 10 km column); a Cb's head reads a wider, downwind-displaced cell field
  above its waist (`headW`, the mushroom overhang); shallow cells get no stem narrowing (`tallC`) and
  "Base flatness" (`high2.w`, 0.8) damps the lobes and the erosion at convective bases (mammatus look).
  **Flow**: bends only the weather map (full flow) and the cluster field — never the cells (any warp
  stretches every field read through it by its shear); "Flow warp" capped at 0.1. **Ground shadow**
  (`cloudGroundShadowV2`): starts 10 m up (on the sea the start point straddled the 0 m shell floor in
  float rounding: rings and lines of shadow around the nadir), reads the field at the step's footprint,
  jittered; 20 steps through the lowest 3.5 km above the ground, then 12 through the rest (review 3: 24
  even steps over the whole shell were ~600 m apart under a high Sun, so each pixel's jitter hit or
  missed a 300-m deck — the shadow of a thin overcast was a grain of full shadow and none; +~0.8 ms). **Sun-colour cache** in the march refreshes on 800 m of altitude too (from inside the anvil
  a ray lit the cumulus 10 km below with the 12 km sunlight: clouds flipped gold to white with 1 km of
  eye altitude). **Night side**: the cloud's sky ambient fades over the Sun's first ~5 deg below the
  sample's horizon (`skyDusk`; skyZenithAt's six midpoints over-counted twilight). **Beam spots**: a
  soft dome (1 - smoothstep(0.25R, 1.15R), x 1.885 = same energy) on the ground and on cloud.
  White balance backs off below ~11 deg of Sun. Open: fine contour ripples on smooth storm domes
  (not the march steps, anvil, mid/high layers or the light march; present before this pass).
- **Pass 15 (2026-09-28, the user's third batch):** **Cumulonimbus is three layers**: the low layer's
  storm regions stop at "Storm cumulus top" (`column.w`, no full-span deep cores while "Cb columns" > 0);
  `cv2ColumnSigma` draws discrete towers on a jittered 3D lattice (`anchorCol`, 64 cells per period so the
  cell hash is stable; a centre is kept within 0.45 cell of the sea-level sphere, and the 2x2x2
  neighbourhood is exact to 0.75 cell), each with its own axis and a concave radius profile: wide base,
  waist, head flare under the lid, blown downwind, with storm-scale lobes. `cv2AnvilSigma` hangs lower
  around a tower's head (`near`). A margin field thresholded per height cannot narrow and flare again, so
  the old Cb was a mountain sloping up into a floating anvil. Every shape constant is a setting (sliders
  164-175: `cb_columns`, `cb_spacing_km`, `cb_radius_km`, `cb_cumulus_top_km`, `cb_waist`, `cb_flare`,
  `cb_head_drift_km`, `cb_lobes`, `cb_sparsity`, `cb_overshoot_km`, `anvil_thick_km`, `anvil_hang_km`),
  so one harness launch can sweep configurations with `set`. Column strength is read once per sample
  from weather mip 3; a read at each candidate centre (up to 8 scattered cube fetches) took storm views
  from 10 to 29 ms. **The flow is evaluated once per field sample** (`gCv2FlowSet`/`gCv2FlowD`, set by
  `cv2Field`): low, mid, anvil, high and columns each paid the eight waves. Columns cost ~1.5 ms in a
  storm view. **Cloud airlight** (`cloud_v2_march.comp`): the path's in-scatter was weighted by
  (1 - attn) as well as (1 - T), counting extinction twice (inherited from v1's camPathInscatter). A
  cloud got a fraction of the haze beside it, and past the terminator from 15-60 km it read as dark
  blue patches in golden haze. It now also takes the sky pass's orbital terminator gate, so the two
  agree above 40 km. "Storm feature size" goes to 12. Open: the dome tips seen from above have
  pits/rings; a thin anvil (< 1.5 km) streaks horizontally at grazing views.
- **Pass 16 (2026-09-29, the user's fourth batch, 12 km towers):** the tower lattice is **2D on an
  equal-angle cube map** of the drifted sphere (2x2 search, 3x3 once a tower's reach passes 0.72 cell;
  only the upper half of a tower can need it). The 3D lattice (pass 15) cut big towers along cell faces
  (grid seams) and rejected most candidates as off the sphere; the 2D one cannot straddle a cube-face
  edge, so towers whose reach crosses one fade out (a tower-free band, never a cut); `anchorCol` is
  unused. **Every tower meets the anvil**; its head rounds off over 1.5 km and stays 300 m INSIDE the
  anvil, and only about a third of towers push an overshooting dome through (`cb_overshoot_km`, 0.8,
  replaced `cb_full_frac`). Heads closing at the lid stood above it as flat plates tens of km wide:
  march-step contour rings, a moat, and from orbit dark "tumours" (a grazing Sun's light-march ray skims
  a plate; decks avoid that through the path-to-top term, towers do not). Tower edges ramp over a fixed
  ~300 m (0.2 x the radius was 2.4 km of fog around a 12 km tower). The cirrus layer (0.69-0.93 of the
  tropopause) gives way over storms (one weather read), since it cut through anvil sides. The march's
  sun-colour cache is kept at fixed 800 m levels and interpolated. The ground height is read once per
  field sample (`gCv2GroundSet`). **Perf is open:** with the user's 12 km towers, 3.6 km anvil hang and
  3.3 km anvil, storm views are ~34-41 ms of cloud march vs ~16-29 ms with towers off. Towers at 0.3
  density cost the same, so it is not the volume, but no per-sample cut tried moved it (3D -> 2D lattice,
  fewer fetches). Also open: the concentric rings looking down from inside the anvil (user snap 1), not
  the sun-colour cache.
- **Pass 17 (2026-09-29, the user's fifth batch, 1920x1009):** the tower lattice's cell is at least
  (full reach)/1.2 (`reachFull`, uniforms only), so the 3x3 search covers whole towers: 12 km towers
  flared 2.6x reach ~55 km, and on 20 km cells everything past 25 km was dropped (heads and hang cut
  on arcs). "Cb spacing" is now a minimum, and the change MOVES every tower (a snapshot framing a
  storm shows a different one). The hang footprint fits the search. A non-overshooting tower's dome
  apex sits 300 m inside the anvil (it was AT the lid for every tower); the column reports its local
  top (the dome's surface over the point — the apex made dark craters via the deck/ambient terms); a
  head's lobes and erosion fade over its last ~1.2 km (their lumps showed through the thin lid as a
  cell pattern). The mid layer's altitude reads the Perlin stretched to +-2 sigma (raw it was one flat
  sheet at ~4.5 km, a dead-straight line across storms seen from near its height) and gives way over
  storms (`CV2Col.storm`). Tried and REVERTED: sinking heads 900 m (+15 ms from above), joining
  towers and anvil by max (thinner storm, +30-50%), a light-march od cutoff + probe skip + opaque-ray
  stride (all three together SLOWER, ~13 ms). Measured (harness = in-app within ~10%): the user's
  pass-16 exe 33 / 64 / 42 ms cloud march at the three snaps; pass 17 30 / 85 / 55. Open: a
  waffle-grid texture on lit anvil walls (not the shape texels: a C1-smooth fetch did not change it),
  floating anvil fragments far from towers, and the storm cost.
- **Pass 18 (2026-09-29):** the corrugated tower walls ("waffle") were the tower strength read from
  weather mip 3 (~40 km texels) by hardware filtering, whose 8-bit sub-texel weights step every ~150 m;
  it scales an 11 km radius, so the wall moved in steps: vertical flutes up the whole tower. Now
  `cv2WeatherBilinear` (four texel-centre fetches blended in float, C1). A tower's radius goes to ZERO as the storm
  strength fades (sqrt(smoothstep(0.02, 0.5, str))): floored at 0.7 of its radius it vanished at full
  density where the strength crossed the 0.02 cutoff — a flat vertical crescent wall ~10 km wide, which
  the march drew with flutes and rings (the rest of the "waffle"). `cv2Add` weights the deck flag
  by density share (max() gave a tower crossed by a thin anvil the deck's path-to-top shadow at the
  tower's density: a black band). Open: wood-grain contour rings on smooth tower surfaces (the pass-14
  "contour ripples", obvious with low lobes); the mid layer's base seen from just below it draws a dark
  grazing band across storms behind it.
- **Pass 19 (2026-09-29):** a weaker tower is SHORTER, not thinner: its top (`topC`) sinks from under
  the anvil at full strength to its base at the 0.02 cutoff, its radius stays >= 0.8, and only towers
  reaching the anvil (`full`) get the waist, flared head and overshoot dome (shrunk in radius, storm-edge
  towers stood as thin "straws"). "Storm cumulus top" is a TARGET for the storm regions' low tops
  (`mix`, not `min`: the congestus type already topped out below it, so raising it did nothing).
  **Full rate only while moving** (`cv2SparseWhenStill`, slot 176, key `sparse_when_still`, default
  on): after 8 frames with no camera turn (> 0.3 half-res px), eye motion, zoom, time warp or history
  loss, the march goes back to 1 pixel in 4 even above `full_rate_above_km`; the history converges on
  the same image. Measured at 1920x1009 overlooking anvils: 16 ms still vs 44 ms full rate; a settings
  change takes ~4x longer to reconverge (grain for a few seconds at history weight 0.05).
- **Storm cells (review 4, 2026-09-29, `cv2CbRole`):** towers come in CLUSTERS: the lattice is grouped
  in 3x3 blocks, a block holds a storm cell with probability "Cb fill" (`column3.w`, now per block): one
  DOMINANT tower (the only one reaching the anvil: flared head, overshoot, the anvil's hang, lightning)
  and a flanking line of 0-2 towers stepping down beside it (height x0.7 / x0.5, radius x0.8-0.9, never
  anvil-reaching). cloud_v2_lightning.comp flashes dominant towers only. A tower's outline BULGES
  (turrets: 3rd and 5th angular orders, turning with height, within ~23% so the lattice search still
  covers it), and a non-anvil tower's top rounds off over up to 3 km (flat boxes at 1.5 km). Defaults:
  waist 1.0 (none), flare 1.3, anvil hang 0.6 km — the waist, flare and hang around EVERY tower made an
  even forest of mushroom-capped smokestacks (user snapshot 7, review 3). The lattice moved again (the
  flare sets the cell). Open: the dominant tower's walls up close are still near-vertical.
- **Storms as clusters, not map regions (review 5, 2026-09-29):** the map types regions hundreds to
  thousands of km across as Cb. (1) The low layer's storm cumulus ("Storm cumulus top") now rises only
  within "Storm cumulus reach" (km, `column3.x` in METRES, 0-60) of a tower's edge — `CV2Col.prox`, from
  the column search, which cv2Field now runs BEFORE cv2FieldLow (`stormProx` argument; 0 for the eye's
  rain) — and away from towers a Cb-typed region is capped at the cumulus type's top. Taken over the whole
  region it was a continent-wide 5-7 km floor with the towers standing in it (user snapshots 1-2). The
  proximity is computed at every height (below the Cb base too: `colH`), or the low tops would step there.
  (2) The towers' flared heads ARE the anvils: the user tuned a "perfect anvil" with the map-fed shield
  OFF, and those settings are the defaults (radius 15 km, flare 1.6, waist 0.7, lobes 0.18, columns 1.5,
  storm feature size 12, fill 0.57, cumulus variation 0.8, anvil thickness 0.95 / hang 2.4 km, `anvil` 0):
  a 15 km tower sets ~42 km lattice cells, so storms stand ~125 km apart. (3) "Cb head lobes"
  (`cb_head_lobes`, slot 207, `motion.z`): the head (zeta 0.5-0.75 up, anvil-reaching towers) has its
  own lobe strength. (4) The map-fed shield (`cv2AnvilSigmaP`, still available) is smooth: fray 0.15
  (was 0.45), a soft edge (presence 0-0.6, squared), a gentler underside — it was lumpy and grey
  (snapshot 5). A separate storm layer not rooted in the map (hurricanes) is a possible later step.
- **Pass 20 (2026-09-29):** "Storm cumulus top" reaches the cloud AROUND a storm: the weather map
  types only a storm's core as Cb (which the towers cover) and the cloud around it as stratocumulus,
  so keyed on the Cb class the slider changed 0.2% of a storm view. Low cloud now takes it by storm
  proximity (weather type at mip 4, float-bilinear, 0.3-0.6), fully for cells, half for
  stratocumulus: 26% of the view moves between 2 and 7 km. The mid layer gives way wherever towers can
  stand (`col.storm` 0-0.15): a mid-layer sheet cut a weak storm-edge tower as a horizontal belt.
  "Storm cumulus reach (km)" (`cb_cumulus_reach_km`, 10, slot 177, `column3.x` (since review 5: metres from a tower's edge) = the weather mip whose texels
  are that wide) sets how far; the storm threshold falls with the mip (0.62 - 0.06 x mip), since a fine mip sees
  the core undiluted and at 0.3 it caught plain cumulus everywhere. Head drift applies to anvil-reaching towers only. The lumpy "fields" beside towers seen from altitude
  were the mid layer (debug view 7 green), not cumulus.
- **Perf sprint (2026-09-29, overnight):** the storm views' cloud march is ~62-67% cheaper at full rate
  (the user's 68 ms overlook: 22.5 ms), images within noise of before (`harness_runs/perf_sprint/`:
  `baseline.json` vs `cp1.json`, captures `cap_ref` / `cap_e14`). **What governs this shader's speed:**
  (1) every call site inlines the whole field (~60 KB of GPU code each): the light march, far sample,
  sky probe and beam occlusion now share ONE call (`lightAmbOD`), and the coarse and fine view samples
  another — 459 KB -> 220 KB, -45%; (2) NVIDIA gives it either 128 registers (small spill) or ~227 (no
  spill), and 227 is 20-60% SLOWER. Trivial edits flip it — a per-sample loop bound, three debug
  counters, values cached in globals — so check `shaders reload`'s Register Count (harness,
  docs/HARNESS.md) before timing any variant, and A/B variants interleaved in ONE live app (run-to-run
  noise is 5-10%). A smaller workgroup forcing fewer registers spills and loses (32x32: 2.4x slower).
  Kept: the one call site; **light LOD** (`cv2LightLodFootprintM`, "Light LOD footprint (m)", slot 178,
  key `light_lod_footprint_m`, 40, 0 = off: behind T < 0.5 or past that pixel footprint, 2 light steps
  over the same length with the first kept — dropping the first step flattened the towers' self-shadow);
  cirrus shaded at COARSE steps (a thin-ice sample stands for two steps and never drops the march into
  fine steps: they were a large share of the steps from altitude); the low field's altitude bound (no
  low cloud tops past the congestus top / "Storm cumulus top" + the ground's lift); `[[dont_unroll]]` on
  the field's small loops. **"Half rate while moving"** (`cv2HalfRateMoving`, slot 186, key
  `half_rate_moving`, default OFF): where the march would run at full rate it takes a checkerboard of the
  half-res pixels, alternating each frame (`cv2.misc.z` = 2; the resolve estimates a stale pixel from its
  four fresh neighbours and pulls a fresh one toward its diagonals in motion): -25% of the cloud bucket
  in storm views, but visibly grainier in motion than full rate (harness_runs/perf_sprint/motionb_c.png) —
  the user's to judge. Tried and dropped: a per-tile tower list (lists of dozens of towers from
  altitude: slower), a shared-memory tile cache of the lattice (slower), a lighting context caching the
  view sample's coarse reads in globals (the register flip), reusing the far sample / probe every other
  lit sample (0%), one call site for view + light (234 registers: +60%). Remaining cost: the towers'
  candidate search (~35% of the rest), mostly coarse steps through broken cumulus out to max distance.
  **From orbit (420 km, ~13-14 ms full rate) the cost is spread over the layers** — each off: Cb
  columns -3.5 ms, mid -2.6, anvil -2.0, high -2.0, light steps 1 -1.8, max_iters 80 -2.3
  (`harness_runs/orbit_lod/attrI.json`). **From inside the high layer (11 km, 2026-10-04)** the cirrus is ~11 of 32 ms: horizon
  rays stay in its band for ~200 km and every sample is evaluated and shaded (density does not change the cost); the
  anvil heads ~3, columns ~2.4. Its bounce term now reuses the field's flow (`gCv2FlowD`) instead of re-running the
  eight waves. Tried: 4x steps for far thin-only samples (-1.2 ms) and a 100-km sun-colour cache (-1.3, sunset
  colours change): not adopted. Tried and dropped: skipping the empty shell above a
  conservative per-column ceiling (tropopause top + overshoot, ground-lifted low tops) — 0% (the
  steps are inside the cloud, not above it); a vertical step cap growing with the footprint (200 -> up
  to 500 m) — -0.4%. An orbit LOD needs a cheaper field (an impostor), not fewer steps.
- **Lightning + thunder (rebuilt 2026-10-05; `cloud_v2_lightning.comp`, `SatelliteSimLightning.cpp`,
  `lightning.vert/.frag`, `include/lightning_draw.glsl`):** WHICH flashes happen is the GPU's: one workgroup walks two
  lattices around the observer out to the cloud tops' horizon — the Cb TOWERS (dominant, anvil-reaching: "Lightning
  (flashes/min/tower)" `lightning_rate`) and STORM REGIONS over every deep-convective, raining region of the weather
  cube ("Lightning rate: storms (/min per 20 km)" `lightning_storm_rate_per_area` 0.5, slot 267, `cv2.lightning2.x`) —
  so whole storms flash, not only their towers. The regions are a FIXED 64 x 64 equal-angle lattice on all six cube
  faces (~156 km, the same at any eye altitude: the first cut's cells grew with altitude, so descending with time
  paused reshuffled every flash); each draws candidates at the full rate at random points and keeps each with the
  storm's activity there (thinning: only live candidates read the weather). **Every live flash probes its column**
  (16 `cv2Field` samples, ground to tropopause + 1.5 km, cloud without rain or the thin high layer > 4e-4/m) and takes
  the DRAWN cloud's base and top; none, or under 1.5 km deep, and it does not flash — the Cb type's heights put
  flashes in clear air above the capped cumulus away from towers (user snap 1). Measured: snap 1 (9.9 km) 27 flashes
  a minute out to ~360 km; snap 2 (609 km) ~10/s over the visible disc. Deterministic in sim time (`cv2FlashIntensity`: a 50-ms stepped leader, then
  1-4 return strokes ~35 ms over a continuing current; in-cloud flashes 2-6 softer pulses). Record (`CV2Flash`, 64 B,
  `kCv2FlashMax` 128): origin, ground point, KIND (0 in cloud, 1 cloud-to-ground ~30% where strong, 2 red sprite, 3
  spider lightning just under the base), seed, age, the cloud's base/top, strength, duration. Ground strokes leave the
  cloud's flank and land 1.5-9 km aside (slanted). HOW they look is the host's: `updateLightningBolts` reads the
  previous frame's list (only a list the pass wrote last frame: `cv2LightningObsDirValid`, one-shot, with the observer
  direction it was written in) and builds each new flash's CHANNEL once as geometry (`buildBoltTree`, a fractal tree
  from its seed: midpoint displacement down to ~3 px at its distance; a ground stroke's in-cloud wander, exit and long
  slanted descent, branches every ~260 m / "Lightning tendrils" (`lightning_tendrils`, slot 268), twigs and many dim
  tendrils, upward streamers at the strike point; a spider's 3-6 near-level arms 6-30 km with a web of twigs and
  drooping tendrils; a sprite's 20-45 forking tendrils and rising streamers), cached by id, and uploads the flashes'
  frames + state (`GpuBoltFlash`: revealed arc — the leader working down, a spider spreading at 150 km/s — the
  branches' share, which fades ~0.12 s after the first stroke, 4 cloud-glow emitters) and the segments (`GpuBoltSeg`,
  32 B, up to 128k). Drawn in the main pass AFTER the sky TAA at full resolution, SCREEN-blended (src + dst(1 - src):
  exactly the sky tonemap 1 - exp(-x) of the summed light, so overlaps saturate instead of clipping): channels as
  antialiased capsules at their TRUE luminous width (3 m main, 1.2 / 0.6 / 0.35 m; sprites 120-380 m) — thinner than a
  pixel they are a pixel wide with their energy kept, never fatter — plus a halo (light scattered by rain and air: 70 m
  main, 22 m branches), hidden by the cloud composite in front (`cloudPointVisibilityAt`; rain shafts included), faded by
  the air COLUMN between eye and point (`boltAir`: a flat haze length left ~5% from orbit); then the cloud lit around
  the flashes in ONE full-screen pass looping over the emitters (per texel of the half-res composite, `boltGlow`: a
  ~4-km diffusion core floored at ~3.5 km and a faint 15-km tail, windowed to 45 km — from orbit a flash is a soft
  10-20 km patch). The lightning pass's thread 1 adds the flashes' light to the rain at the eye (`cv2RainAmb`): drops
  light up in a close strike. **Thunder from the channel** (`queueThunder`, within 35 km): each segment's sound arrives
  at its distance / 343 m/s, weighted by length, 1/r and how far it lies across the line of sight; binned at 50 ms that
  envelope IS the roll, handed to the `thunder` synth through `AmbientSynth::pushEvent` (a lock-free queue; the noise
  low-passed by the distance the sound has come, which grows through the roll; a ripping crack under ~3 km).
  `updateThunder` only plays the queue. Harness: `lightning` (flashes with kind / segments, `trees`, drawn counts,
  thunder queue), `lightning spawn kind=cg|ic|spider|sprite dist_km= az= [seed=] [base_m=] [top_m=] [ground_km=]`
  (heights above the eye's ground; injected flashes run the same channels, glow and thunder), scripts in
  harness_runs/lightning (spawn2: a clear desert night; t5: orbit + aircraft at 5 N 90 W). Cost ~0.3 ms for the
  lightning draw. It REPLACED (2026-09-29 .. 10-04) `lightningCS` / `spriteCS` in cloud_march.comp: a 14-vertex channel
  with fixed branches and the glow, per pixel inside the half-res composite (smeared by the sky TAA, a 3-m minimum
  width = pudgy), and only dominant towers flashed (a map-typed storm without anvil towers had none: user snapshot 43).
- **Weather evolution (2026-09-29, decision A):** the weather cube is re-baked as sim time moves
  (`SatelliteSim::recordWeatherEvolution`, one face per frame with all its mips, once it lags by ~300 m of
  wind drift or the settings change; the bake's pipeline and per-mip sets are kept, `cv2Wx*`). The bake
  (`cloud_v2_weather.comp`) reads the map through TWO copies advected by a slow curl wind (`evoWind`,
  ~4000 km, turning over a day), each over its own window P offset by P/2 and weighted sin^2 of its phase
  (0 at its reset): the flow-map double-phase trick, so the map moves locally and never drifts from itself
  (displacement <= wind x P); and its coverage grows and decays under a smooth field (`evoGrowth`, ~1500 km,
  10-30 h), so the types it classifies — storms included — strengthen and weaken. Everything downstream
  (towers, anvils, lightning) follows. Settings: "Weather evolution wind (m/s)" `evo_wind_mps` 8 (0 =
  the static map, exactly as before), "Weather growth / decay" `evo_growth` 0.12, "Weather evolution
  window (h)" `evo_window_h` 3 (slots 182-184). Cost: a face bake is ~0.1 ms (not measurable at 1 h/s).
  With it on, a snapshot's moment shows the map moved up to ~86 km and re-shaded: an older snapshot frames
  a slightly different storm. The rigid drift (`cloudDriftPhase`) is unchanged and on top.
  **Afternoon land convection** (`evo2`, "Afternoon land convection" `evo_diurnal` 0.6, slot 185): the
  bake raises coverage (+0.2) and pushes the type toward congestus / Cb (+0.3) over land where the Sun of
  THREE HOURS AGO stood high (the ground heats through the afternoon; `weatherEvoPC` turns the sim's
  Sun back 45 deg and into the map frame) and the map has cloud nearby (its moisture proxy): continental
  afternoon storms, dying back after dark — the one-moment map carries no diurnal cycle of its own.
  **The flat stand-ins read the same cube**: the sky set's binding 7 is the weather cube (a `samplerCube`
  written by `writeCloudsV2ConsumerDescriptors`), not the 8K equirect map, so `sat_sky.frag`'s flat layers
  (the mirrors' and env probes' clouds, SKY_LITE) and Potato's deck show the evolving map the volumetric
  clouds are built from (the Earth-fixed direction turned by the layer's drift into the cube's frame, one
  mip coarser than the equirect's). The mesh shaders' `earth_env.glsl` fallback keeps the 2D map.
- **User feedback round (2026-09-29, after the sprint):**
  **Lightning** toned down (glow x2, bolts x60 in `lightningCS`; were x5 / x150). **Bolts (review 4):** a
  FRACTAL tree built in one sequential pass per pixel (the main channel's random walk carried along, not
  re-summed per vertex): a branch from ~60% of the main vertices (6 segments, angling down and out,
  shrinking with height lost) and twigs off ~55% of branch vertices (4 segments); each segment
  (`boltSeg`) draws a core whose PEAK is capped (so an overexposed stroke keeps a pixel-ish width instead
  of swelling into round-capped tubes whose radius jumped along the channel) plus a faint halo, dimmed
  SMOOTHLY where it passes behind the cloud; a bounding test skips rays far from the tree. Default glow
  0.088 (the user's: at 1 the flash's cloud glow drowned the bolt). Cost ~0. **Red sprites** (easter
  egg, "Lightning sprites (chance)" `lightning_sprites` 0.05, slot 191, `misc2.x`): a ground stroke may
  set off a sprite ~75 km up (flash list kind 2, `spriteCS` in cloud_march.comp: a deep-red head,
  8-13 tendrils to ~40-50 km, each forking twice as it falls and spreading out (`spriteSeg`), the tips
  leaning violet, ~0.1 s; review 3 made it darker and branched — 5-8 unforked tendrils read as a comb); thunder and the harness skip/label it.
  **Cb columns**: "Cb fill" (`cb_fill` 0.5, slot 189, `column3.w`) — the share of lattice cells holding a
  tower at all (the user's sparsity ~0 put one in every cell: an even grid of columns); centres jitter
  over 0.15-0.85 of a cell (was 0.25-0.75), so the 2x2 search is exact to 0.65 cell and the 3x3 to 1.15
  (`limM` 0.62 / 1.12, cell >= reach / 1.12) — this MOVES every tower again; "Cb tower radius" reaches
  20 km. `cloud_v2_lightning.comp` applies the same fill and jitter. "Storm cumulus variation"
  (`cb_cumulus_variation` 0.3, slot 190, `column3.z`): the storm cumulus top varies +- that fraction by
  region (the cluster Perlin). **Motion**: WASD moves ~0.08 rad/s of arc (~510 km/s, boost ~3200 km/s),
  i.e. ~17 km a frame, so the history is useless and a frame is one march — whose IGN start jitter lined
  neighbouring rays' steps up into corduroy bands ("clouds lose all form"; a 550 m/s harness path shows
  nothing wrong). When the eye moves > 150 m in a frame (`misc2.z`) the march jitters from a 64x64
  void-and-cluster BLUE-NOISE tile (`makeBlueNoise64`, built at init, march binding 15, SSBO) + a golden
  step per frame, and the resolve blends toward a 3x3 tent of this frame's samples as the parallax passes
  6-24 px (or with no valid history). White noise was tried first: clumpier. Harness:
  harness_runs/motion2 (fastG = before, fastG4 = after, paths at the real walking speed).
- **Adaptive rate / dynamic sampling (2026-09-29, `cv2AdaptiveRate`, "Adaptive rate while moving (0/1)",
  key `adaptive_rate`, slot 192, default ON; "Adaptive parallax (px)" `adaptive_parallax_px` 1.0, slot 193,
  `misc2.w`; rate `misc.z` = 3):** where the march used to go full rate because the view moved, it runs the
  sparse grid (pass A, `cloud_v2_march.comp`) and marches at full rate only the 32x32 half-res tiles whose
  clouds show more than that parallax a frame (|eye delta| + the volumes' slide over last frame's resolved
  depth, march binding 16) or whose history comes from off screen (a pan's uncovered edge). A classify pass
  (`cloud_v2_tiles.comp`, one 16x16 workgroup per 32x32 tile, same set) lists them (`TileBuf`, binding 19:
  indirect args + tile list + flags, reset by fill/update each frame) and pass A returns on a listed tile; pass B (`cv2MarchPassBPipeline`, the same shader with
  spec constant 2 = 1, `vkCmdDispatchIndirect`, 4 workgroups per tile) marches them into `cv2FullImg` /
  `cv2FullDepthImg` (bindings 17/18) AND writes their sparse-grid samples, so the quarter grid stays
  complete; the resolve (bindings 6-8) takes a listed tile's pixels as fresh (`tileFull`, `freshAt`).
  Why tiles and two passes: skipping pixels inside a full-rate dispatch saves nothing (a warp costs its
  slowest lane). Not used without history, in fast flight (> 150 m a frame: full rate + blue noise) or
  with a workgroup size other than 16x16. **The vote was first inside the march, and it (together with a
  one-line `dbg != 11` exemption) flipped the march to ~224 registers: +19% at full rate** — it is its own
  pass for that reason, and debug view 11 reaches only the resolve (`fog2.z`; the march sees view 0). The
  off-screen test has NO margin: a pan uncovers ~2 half-res px a frame, and a 1% margin (~10 px) meant
  no edge tile ever voted. Measured at 128 registers (harness `path play` reports `gpu_ms_mean`): storm
  views flying / panning 20.2 -> 9.5 ms, 22.5 -> 7.9-10.6; light views ~equal — there the frame is set by
  the longest (horizon) rays, so sparse and full cost about the same (a still anvil view: 6.0 vs 6.2 ms).
  Images match full rate (mean |diff| < 1/255, <= 0.2% of pixels past 12, no tile seams at 8x). At 550 m/s
  nothing passes 1 px (the deck below is ~0.5-1 px). Debug view 11 tints the full-rate tiles red. Harness
  `state` reports the rate (`clouds_v2.rate`: sparse / full / half / adaptive).
- **Fog/dust/ice fog review (2026-09-29, the user's snapshots):** (review 2: the variation is GENTLE —
  regions 0.3-1 at ~500-1500 km, plumes 0.35-1 with a wide threshold; hard-edged regions x plumes read
  as distinct blobs; the ice fog's Antarctic term needs land, it covered the Southern Ocean) dust is patchy by REGION
  (`cv2DirNoise`, an analytic value noise on the direction at ~300-1000 km: a dust event covers a region,
  the next stays clear; the whole dry continent sat at one level) with a lower plume floor, and fades
  seen from above (x0.25 by 80 km of eye altitude: mostly terrestrial). Its exponential height profile
  (and the ice fog's) is integrated EXACTLY over each march step (a sample at one height per step drew
  the step pattern as rings on the ground from 70 km), steps crowd at the far (ground) end when the eye
  is above the band, and the 150-km cap keeps the ground end from above. **Ice fog / diamond dust**
  ("Ice fog amount" `ice_fog_amount` 1, slot 204, `fog2.w`; "Ice fog density (1/m)" `ice_fog_density`
  0.0002, slot 205, `sunE.w`): over Antarctica, Greenland's high interior and the high Arctic's land
  under a clear sky, scale height 250 m, patchy; lit with the ice optics at a halo-rich habit (22/46
  degree halos, sundogs, parhelic circle, CZA) plus a sun pillar (`cv2PillarOptics`), Sun or Moon.
  Knockout 2048 switches it off with the fog. Harness: observer lat=-78, `camera look sun`.
- **History weight moving** ("History weight moving" `history_weight_moving` 0.7, slot 203,
  `motion.y`): the resolve's new-sample weight once the view has parallax (from a quarter half-res
  pixel), with a variance clamp (mean +- 1.25 sd) in motion. "History weight" (0.05) stays the still
  view's: at 1 each pixel alternated between its own ray and its neighbours' estimate (a checkerboard
  flicker), and the fixed 0.35 ceiling in motion let small clouds ghost (user snapshot 4). How it moves
  is the user's to judge; a harness path over 150 m a frame is the fast-flight mode, not this.
  **The still weight is capped at 0.15** (slider and settings load, review 2): at 1.0 the user's clouds
  were a woven checker and deck tops showed "wood grain" (snapshots 4, 6); 0.3 still weaves visibly.
- **Fog + dust (2026-09-29, replaced v1's `fogMarchCS`, a global 1.4 km noise haze):** `cv2FogDust`
  (clouds_v2.glsl), driven by the weather cube. FOG over the SMOOTHED ground (`cv2Ground`, ~15 km), so
  valleys fill deeper and ridges stand out: radiation fog on clear-to-broken nights in valleys (ground
  below its ~80 km mean) and on low ground by the sea, burning off by ~12 deg of Sun; sea / advection fog
  on the coast and at sea under a stratiform regime, day or night; light mist in rain; patchy on the
  cluster field; its top undulates with the low shape noise (a flat sheet read as a painted plane). DUST
  over dry land (the map clear over ~80 km, not sea, not rain) in plumes on the coarse cluster field,
  falling off with height; tan albedo, forward phase, a diffuse multiple-scattering term (without it a
  dusty sky read as a grey veil). **Not part of `cv2Field`:** any addition to the main march loop — even an
  unused `CV2Field::dust` member — flipped the march to ~224 registers. They get a 20-step march of their
  own AFTER the clouds' (u^2 spacing through their band, cut at the scene depth and 150 km), lit cheaply
  (the key light at the band's middle x the map's cover overhead x the path to the fog's top; sky zenith;
  Moon and city light at night), composited by distance around the clouds' mean depth. Cost ~0 at the
  benchmark views; from above the band it is skipped where the cloud's T < 0.01 (2026-10-04: its 20 steps under
  opaque cloud were ~3 ms of an orbit storm view, user snap 1; image within run-to-run noise). Settings (section "Fog & dust", slots 194-199): `fog_amount` 0.6, `fog_depth_m` 250,
  `fog_density` 0.012, `dust_amount` 0.5, `dust_height_m` 1500, `dust_density` 0.00025. Knockout bit 2048
  (was "fog layer"; the Low / Planetarium / Potato presets set it) switches both off. Harness scenes:
  harness_runs/fogdust (Po valley at sunrise from 2.5 km is the reference look). Not yet: fog in the light
  march / ground shadow / beams, wind-driven dust storms (needs a wind field).
- **Light volume + godrays (2026-09-29, EXPERIMENTAL, off by default):** `cloud_v2_lightvol.comp` bakes a
  camera-centred 128 x 128 x 32 R16F volume of sun transmittance (march set bindings 13 storage / 14
  sampled; +-"God ray range" `godray_range_km` 400 about the eye, sea level to 16 km, gnomonic columns; 4 of
  its 32 levels per frame, each voxel 16 growing steps toward the Sun, ~127 km) — the plan's phase-3 light
  volume, so far feeding only the godrays. "God rays" (`godrays`, 0 = off and no bake, slot 187; range slot
  188): for SKY pixels the march scales its output transmittance by the share of the ray's single-scattered
  airlight that lies in cloud shadow (16 samples + a 4-sample tail to the atmosphere's exit), so the
  composite dims the sky by it. Two first cuts failed: SUBTRACTING the shadowed airlight (negative radiance
  from the march's air model, which does not match the sky pass's) overshot to black under an anvil at
  sunset; scaling T on surface pixels darkened the sea, which already has its own cloud shadow. As it
  stands the effect is subtle (a low Sun's airlight comes mostly from beyond the volume) and unjudged —
  real shafts need a dedicated jittered sub-march over the volume's reach — NOT the sky pass's N_VIEW
  loop, whose ~10 km steps (100 km / viewSamplesMin) are far coarser than a shaft, and which would take
  `sat_sky.frag`'s 16th and last sampled-image slot besides. Ground shadows and beam shadowing
  could read the same volume later.
- **Soft ground contact (pass 10):** the march fades extinction over the last max(40 m, 3 pixel
  footprints) before the half-res scene depth: dense cloud meeting a slope ended on a hard,
  stair-stepped line.
- **Rain-only samples stand for four coarse steps** (2026-10-04, `rK`): each runs a light march whose steps evaluate
  the whole field (towers included). Under a snowing storm at night (user snapshot, Dushanbe, 2 km) the cloud march went
  40.6 -> 27.2 ms; in review 22's daytime storm 8.3 -> 6.8, a shaft's edge ~3 levels brighter. Tried: skipping the
  rain light march (-20 ms, but sunlight would reach shafts under thick cloud again), reusing it over 1.5-4 km of rain
  (222 registers).
- **Rain + optics** (2026-09-28, `.plans/ATMOS_OPTICS_AND_STORMS.md`): rain shafts are cv2FieldLow's
  below-base branch (`CV2Field.rain`; the shell floor is 0 m while Rain > 0); rain at the eye drives a
  streak overlay in cloud_march.comp (after the resolve, so it animates). Halos / sundogs / parhelic
  circle / circumzenithal arc (`cv2IceOptics`) and the bows (`cv2RainOptics`) are Snell's-law positions
  per colour channel added to the single-scatter phase of ice (`thin`) and rain samples, per RAY in the
  march, for the Sun or the Moon. Debug views 5 (rain optical depth) and 6 (rain at the eye, streak
  mask). Ice optics are gated per REGION by crystal habit (`cv2IceHabit`, evaluated at a ray's first ice
  sample): a 22 deg halo in ~1/3 of cirrus, sundogs less, the CZA/parhelic circle rarely, 46 deg very rarely.
  The sun path to every cloud sample is the Chapman column (`sunTransmit`, include/atmosphere.glsl) plus
  ozone: a 12-step midpoint rule along a grazing path misses the tangent point's dense air.
- **Tonemap controls** (Clouds tab, lighting section; `clouds_v2.exposure_ev / highlight_rolloff /
  white_balance / auto_exposure`, UBO `exposureScale / highlightRolloff / whiteBalance`, the old cloudsV2
  pads): **one GLOBAL exposure** (`globalExposureEV()` = Exposure (EV) + the metered auto offset) scales the
  sky's radiance, the point sources (the point style's ref/limit magnitudes shift by `exposureGainMag()`,
  like the zoom gain) and the Milky Way / zodiacal light; `skyExposure()` mirrors it. **Auto exposure**
  meters the displayed frame (`recordScreenshotCopy`: the central 60% blitted to 64x36, read next frame in
  `readExposureMeter`), steps toward a linear mean of 0.32 and harder when > 4% clips, only DARKENS (to
  -3 EV x "Auto exposure (day)") and only by day. Harness `state` reports it under `exposure`. Highlight
  roll-off blends the tonemap toward 1 - 1/(1 + x + x^2/2) (same toe, long shoulder); White balance adapts
  to the sunlight's colour at the observer (a low Sun is yellow; without it every cloud read beige).
- **The Earth from space vs the Artemis II photographs (2026-09-30, `harness_runs/artemis_ref/`, Wikimedia
  Commons JPGs).** Measured: in the photos sunlit cloud is ~10x the open sea in linear light; the sim showed
  ~2.5x (a sky-cyan sea under a milky veil). The raw sea surface was right (debug view 43: navy); the cause
  was exposure + tonemap. (1) **Auto exposure spot-meters the lit Earth from altitude** (`readExposureMeter`,
  100 -> 1500 km ramp): the mean over pixels brighter than 0.08 (the whole-frame mean counted black space, so
  a small bright Earth never darkened) toward 0.42 (the photos' lit-Earth mean in the meter's units), clip
  counted against the lit pixels, down to -5 EV, gated by lit Earth in view instead of the Sun at the
  observer's nadir (the night-side crescent blew out). (2) **Orbit colour grade** after the tonemap
  (`sat_sky.frag`, `cloud.taaJitter.z` = "Orbit colour grade" x a 30 -> 300 km ramp, slot 219, key
  `clouds.orbit_grade`): y = 1.16 x^2.2 display-linear (fitted to the photos' values) + 30% desaturation.
  **Only on the sunlit Earth** (review 11): weighted per pixel by the Sun at the surface point (the tangent point for
  a ray that misses), over -6 .. +3 deg; on the night side its x^2.2 crushed city light, moonlit cloud, airglow and
  aurora ~10x (the night side from orbit read nearly black).
  The Mie and Rayleigh gains had NO effect on the veil from 70,000 km — do not chase it there.
- **Rainbows are done** (user-approved 2026-09-28): they show at storms on the terminator (low Sun behind
  the observer); a harness run that does not frame one is a location problem, not a render one.
- **Target A's alpha is a signed distance in KILOMETRES** (`include/cloud_occlusion.glsl`): >= 0 opaque
  cloud, < 0 the mean distance of translucent cloud. In metres an RGBA16F alpha was +inf past 65.5 km, so
  from orbit clouds dimmed every satellite in front of them. Point sources use `cloudPointVisibility()`.
- `sat_sky.frag` upsamples the half-res clouds joint-bilaterally where a surface edge crosses the half-res
  grid (the ridge/cloud stair-steps).
- **Passes** (inside the `cloud_march` timestamp bucket, `recordCloudsV2`): `cloud_v2_march.comp`
  marches one pixel per 2x2 half-res block per frame; `cloud_v2_resolve.comp` reprojects history
  (exact: rotation about a known eye + the eye's ECEF delta), clamps, and blends at `look2.w` (0.1)
  rising to 0.35 with reprojection motion; the clamp box WIDENS for a still view (a tight box against
  the quarter-grid neighbours made blocky 2x2 squares in far cloud); the result is copied to history
  and cloud_march.comp composites it. **Full rate from altitude** (`misc.z`, `cv2FullRateNow`, above
  `full_rate_above_km` = 30): every half-res pixel is marched every frame — from orbit the sparse
  march's history is invalidated by motion and the fallback was a 1/8-resolution upsample (the
  "pixelated mess" from space). The march targets are half-res sized for this; sparse frames fill
  their quarter-size top-left. Riding the ISS at 1x is clean (harness_runs/cloud_v2_o). sat_sky.frag skips flat layer 0 (unless the march is knocked out, bit 32768) and the
  3x3 cloud blur.
- **Jitter per VISIT, not per frame**: a sparse pixel is marched every 4th frame, and 4 x 0.618
  stepped its golden-ratio offset by ~0.47 per visit, so each pixel of a 2x2 block kept its own two
  clusters of offsets and its own bias — far clouds (long steps) showed 2x2 squares.
- **The march**: steps grow with distance FROM THE SHELL ENTRY (and the max distance is measured from
  there — from orbit the Earth is further than the limit); the switch from coarse to fine steps lands
  on a jittered lattice (without it a flat deck top drew contour rings around the nadir). When the back-up
  to the fine lattice passes the segment start it restarts JITTERED there: clamped to the start itself,
  every ray from inside a cloud or rain stepped one lattice from the eye (contour rings of the cloud base
  looking up through rain, review 3). A ray that runs out of budget (`misc.x`,
  presets 160-640) is FILLED with the path's mean extinction and the cloud's mean colour, not left
  transparent (that was the horizon "seeing through" the clouds).
- **Key light**: the Sun, or at night the Moon through the same light march (`misc.y` x `moonGain`);
  sky ambient is occluded by the cloud above and by the sample's own density (detail on shaded sides).
  **Night (review 5):** the ambient carries the terrain's moonless "Night sky light" (`terrainErosion.z`,
  x 0.2 onto the moonlit-sky scale), ungated like the terrain's; and `sat_sky.frag` keeps the city glow's
  in-scatter IN FRONT of the clouds (`accumCityFront`, up to the cloud's distance from `cloudA.a`) out of
  the cloud's attenuation: `color * A + B + cityGlowFront * (1 - A)`. The composite multiplied the whole
  night sky by the cloud's transmittance, and the march's airlight has no city glow, so the clouds along a
  night horizon were a black, stair-edged band darker than the ground under them (snapshot 6: band 24 ->
  38, ground 37). **Review 6:** the split distance is the NEAREST cloud of the four half-res texels
  (`textureGather` of the alpha: the filtered alpha blended a cloud's distance with the no-cloud -60000 km
  and changed every frame — a black flicker on night cloud edges), and the term fades in over 40-150 km
  of cloud distance (a near cloud's resolved distance is noisy and moved the split through the dense low
  air; from above most of the glow is below it anyway). Still-frame flicker 1.2% -> 0.27% of pixels
  (0.19% without the term).
- **Review 7:** **Shadowed air is sky-lit.** Air in the Earth's shadow gets isotropic in-scatter of
  half the zenith sky's radiance (faded as the clouds' sky ambient, `skyDusk`), in the march's airlight
  (shadowed steps, `sR`/`sM`) and in sat_sky.frag's loop (`shR`/`shM`, `skyZenithSky` at the shadowed
  stretch's mean point), off from orbit (x (1 - terminator gate)). Single scattering left it black:
  facing away from a just-set Sun, the far clouds along the horizon were a near-black band (user
  snapshot: 18 against ~70 for the clear horizon; now ~40, the clear horizon 65 -> 84). What remains is
  real: those clouds hide the still-sunlit air beyond them. Day views unchanged (mean |diff| 0.05).
  **Motion grain:** the march's blue-noise jitter now switches on at 1 m of eye motion a frame (was
  150 m): with a moving history weight of 0.7-1.0, IGN's regular structure showed as a dotted
  honeycomb on lit tops during a slow strafe. The resolve, as the moving weight rises, gives the
  refreshed sparse pixel the smoothing its three interpolated neighbours have (half its four axial
  neighbours) and the same weight (`mk` also from `wNew`); full-rate pixels lean toward their 3x3 tent
  by 0.4 x mk x w. At "History weight moving" 1 in sparse tiles the image is still single-frame rays:
  grain, no longer a checker.
- **Review 8/8b:** **The air in front of every cloud is the sky pass's** (`airFront` in sat_sky.frag,
  accumulated in its atmosphere loop up to `tAirFrontM` — the four half-res texels' distances weighted
  by bilinear share x opacity, each step by its covered fraction — and composited x (1 - A)); the march
  outputs `attn * L` only. Its own 8-step airlight read far darker than the sky's at a grazing Sun
  (horizon clouds were dark silhouettes 100-600 km out), and a distance split between the two (review 8)
  disagreed at cloud edges: dark 2x2 specks and flicker along silhouettes. **Blue-noise jitter always**
  (per visit): the history's residual of IGN was a fine crosshatch over still clouds. **"History weight
  moving" <= 0.5** (default 0.4; load clamps): at 1.0 motion showed the quarter grid's raw rays.
  **Low bases over plateaus:** the regional ground (weather alpha mip 4, ~80 km, never above the local)
  lifts in full, only the relief above it at 0.6-0.9 — over Tibet the decks sat inside the plateau (a
  clear disc with a ring rim from orbit). **Ground shadow** (`cloudGroundShadowV2`): the field without
  the far-field substitution (`gCv2NoFar`), rain curtains at 15%, read footprint <= 3 km (coarser, the
  weather mips drew straight-edged squares), and past the terminator it marches along the horizon (the
  sun ray into the Earth met no cloud: a hard line along the ground terminator). The far field has a
  16-km octave. Harness note: with `auto_exposure` on, frames after `wait settle` still drift in
  brightness — set `clouds_v2.auto_exposure 0` before measuring flicker.
- **Review 9:** the air split's distance ignores texels with a 0 distance (resolve-blended edges) and
  falls back to the nearest real cloud (not the filtered alpha); the resolve clamps history in MEAN-COLOUR
  space (rgb / opacity, `cv2ToMean`); cloud_march.comp replaces an isolated cloud texel by its 3x3 tent
  and leans edge texels 60% on it; a thin (cirrus) sample stands for two steps only past a 60 m pixel.
  The flickering specks along cloud edges seen from 9 km at 62 N were LOW cloud glimpsed through gaps at
  the edges of the high layer (which the eye is inside there), in its shadow: reduced ~40%, not gone
  (high layer off removes them). **Ground shadow:** a 2x2 ORDERED start offset (was white noise: grain)
  with sat_sky.frag's shadow blur a [1 2 1] tent at 1 texel, which averages a period-2 pattern exactly.
- **Review 10 (2026-09-30):** a texel with cloud but NO distance (the resolve blended history's cloud into a
  texel whose own sample found none) takes its neighbours' opacity-weighted distance (cloud_march.comp);
  an OPAQUE texel stores max(tHalf, mean distance) — along a grazing path through a cirrus veil the half
  point landed IN the veil (~20 km) in front of a cumulus at ~55 km, and the sky pass split the air there
  (every point source is 100+ km away, so their occlusion is unchanged). The cirrus shadow on lower cloud is
  delta-scaled harder in the light march (`1 - 0.8 thin`, g ~0.8: real cirrus barely dims what is under it),
  and seen from INSIDE the high layer (view footprint 4-20 m) the fibres give way to the layer's mean haze.
  Specks along cumulus edges seen from inside cirrus (user snapshot, 9 km at 62 N) are reduced, not gone:
  debug views 46-49 (harness `debugview cloudairsplit|cloudtrans|cloudrad|cloudalpha`) show where the air
  split steps. Tried and reverted: w^2-weighted mean distance, a dark-outlier despeckle in cloud_march.comp
  (both global, neither measurably helped here).
- **Review 12 (2026-09-30):** **the resolved DEPTH has history** (`cloud_v2_resolve.comp`, binding 5 is
  read back: a still view, motionPx < 0.5, blends 0.1 of the new distance): a sparse block took its one
  sample's distance, so the sky pass's air split (`tAirFrontM`) moved on the 4-frame cycle and the air in
  front of the clouds flickered — from orbit, near the limb, while still (std > 5: 2.6% -> 0.13% of pixels).
  The resolve's motion also has an EYE-PARALLAX floor (the eye's move across the ray at the pixel's depth,
  capped at 50 km): moving up/down, a near beam shaft in front of far cloud ghosted. Point sources test
  cloud per TEXEL (`cloudPointVisibilityAt`, four gathered texels weighted bilinearly): the filtered alpha
  mixed a cloud distance with the no-cloud sentinel and dimmed the AI ring's satellites at cloud outlines.
  Aurora from orbit: the dark-sky gate opens over 40-100 km of eye altitude and the extinction starts at the
  shell's inner sphere (looking down, the path to infinity ran into the ground). City night: arterials
  continue to a 150-m footprint (was 10) with their share fading 4-150 m (the street lines stopped in a ring).
  Intro: vantage 300 m SSW (the eroded ridge filled the right of frame); Q/E starts from the ground
  (an offset left below it, 0 after the intro, had to be climbed out of invisibly).
- **Review 13 (2026-09-30):** **opaque cloud texels store the MEAN distance** (cloud_march.comp `occKm`; the
  half-opacity point only when there is no mean). The sky pass splits the air at that distance, and opaque
  texels used max(tHalf, mean) while translucent ones used the mean: tens of km apart along a grazing
  horizon, so the bright air in front of far clouds stepped wherever a texel's opaque/translucent class
  flipped — hard 2-4 texel blocks at any resolution (debug views cloudairsplit / cloudalpha showed it;
  transmittance and radiance were smooth). The mean is the transmittance-weighted origin of the cloud's
  light, i.e. the right split. Also: the march's ray moves within its texel each visit (an R2 sequence,
  not in fast flight) so the history integrates the texel's footprint.
- **Review 14 (2026-09-30):** **no dust or ice fog under the ground** in the fog march (`pp.h < cv2Ground - 30`;
  their height terms come back clamped at 0, so a sample below the surface read the densest value). Past the
  detailed terrain march's range the scene depth lands below the real surface, and from 180 km over the Ronne
  ice shelf the ice fog whitened everything beyond a hard ring ~550 km out (user snapshot 3; the radiation fog
  is exempt — it pools below the smoothed ground in valleys). With no surface at all the march ends at the
  sea-level sphere (it ran to the band's exit on the far side of the Earth). The Antarctic ice fog's land gate
  counts everything south of ~78 S as ice and tests the regional (~80 km) ground elsewhere (the shelves read
  ~0 m and were "sea").
- **Review 15 (2026-09-30, temporal stability):** **the resolved DEPTH's history is gated on the texel's whole
  shift, rotation included** (`shiftPx`): review 12 gated it on the parallax alone, which a pure turn leaves at 0, so
  while the camera PITCHED each texel took 90% of the distance of another direction (sky above the horizon, a cloud
  300 km out below it) and the sky pass split the air there — horizontal streaks and dark shapes along the horizon
  (user screenshot; yaw pans hid it, the horizon's structure being horizontal). On the sparse grid fast motion leans
  on the bilinear estimate, not the block's one sample (`blur` was `nc`: 4x4-pixel squares). Measured with
  `tools/harness/tstab.py` (docs/HARNESS.md "Temporal stability": player-speed paths vs settled references; pitch
  mean |diff| 5.4 -> 1.3, > 16 levels 10.9% -> 0.2%); the harness's `path play record=` had rendered ~12 frames per
  recorded frame (it waited on the PNG encode), which is why no earlier motion test saw the ghosting. Lowering
  "History weight moving" changed nothing measurable; the adaptive rate below 30 km (full_rate_above_km 0) cut the
  boost error 1.5 -> 1.0 at ~3x the cloud cost (8 -> 24 ms): not adopted.
- **Review 16 (2026-09-30):** **the light march has its own jitter** (another cell of the blue-noise tile, its own
  golden step): it took the VIEW march's, so a sample's depth inside a flat cloud top and its light steps' positions
  moved together and their product never averaged out — contour rings round the nadir on the Cb heads' lids from
  16 km (4 light steps over 2.5 km; light steps 8 hid them, the view step's size moved them, the storm noise did not).
  The heads still SHIMMER at a grazing Sun (two settles of one view differ by 7.4 levels at 4 light steps, 5.3 at 6,
  4.5 at 8, 0.8 with the towers off; the light LOD and lightning are not involved): each long light step lands in the
  lit skin under the top or above it. Medium's light steps are 6 (were 4). Tried: lighting heads by the path to their
  top — no effect at a grazing Sun, reverted. A variance-adaptive still history weight (x0.4 / x0.25 where the 3x3
  new samples are noisy) measured within run-to-run noise (4.85 / 5.29 vs 5.3): reverted.
- **Review 17 (2026-10-01): the "blocky far reflections" at sunset were the CLOUD SHADOW on the sea** (debug view 50,
  `oceanshadow`; knockout 256 removed them): (1) `cloudGroundShadowV2`'s dense stretch ended at 3.5 km / max(sun up,
  0.05), ignoring the Earth's curvature — ~70 km out and ~400 m up near sunset — so a deck at 1-2 km fell to the upper
  stretch's ~30 km steps and was hit or missed per texel offset (rows of unshadowed texels: a stair-stepped bright band);
  now the curved-ray distance to 3.5 km. (2) A low Sun's ~100-km shadow path made any coverage over the clear threshold
  full shadow, i.e. a binary map of the source JPEG's 8x8 blocks: the shadow reads the weather map coarser as the Sun
  drops (`gCv2WxLod`, up to mip 3, C1; written only by the shadow, so the march folds it away — 128 registers).
- **Cloud morphology (review 17, 2026-10-01; `tools/make_cloud_morph.py` -> `assets/textures/cloud_morph.rgba8`,
  march binding 20, `CV2_MORPH_BINDING`, UBO `anchorMorph`/`morph`, sliders "Morphology (orbit)" 0.8 / "(near)" 0.5 /
  "period (km)" 320, slots 234-236):** four tiling channels made from scratch (FFT noise, Voronoi on a torus),
  rank-equalised to uniform so a threshold at 1 - cov covers cov: R closed cells, G open cells, B streets, A clustered
  cumulus. `cv2MorphZ` picks them by type (decks closed; convective over the sea poleward of ~30 deg open; streets in
  some regions; cumulus fields elsewhere) through a triplanar cube projection. Far field: the morphology replaces the
  Perlin share and the map sets the cloud FRACTION over ~20 km (the early-out's mip-2 read `wCoarse`, half with the
  5-km cov; decks capped at 0.9 so cell rims open) — thresholded on the 5-km texels it moved 3.5% of an orbit view's
  pixels. The mid layer's regime takes the closed cells too (one read). Only the view march binds it (the shadow,
  beams, lightning, light volume keep the Perlin). **A second weather read for the fraction flipped the march to 228
  registers** — reuse `wCoarse`. From orbit much of what shows is the mid and high layers stacked over the low: the
  low layer's cells show (faint honeycomb, `mid_amount 0 high_amount 0`); the map's own hole shapes still dominate.
  Open: the high layer (cirrus regime) is still Perlin; the morphology is not wind-aligned (streets follow the cube face).
  **Benchmark** (`tools/cloud_stats.py`: cloud mask by brightness - saturation, Otsu; perimeter-area dimension D, object
  size exponent b, the MASK's spectral slope, holes per 1000 km^2). Eight public-domain MODIS scenes (Wikimedia Commons:
  open/closed cells, streets, popcorn cumulus, a frontal band; harness_runs/cloudref, not committed): D 1.48-1.72, mask
  slope 1.5-2.3, 3-16 holes / 1000 km^2, thousands of objects. Ours, nadir from 420 km at five broken-cloud places
  (harness_runs/fb24/nadir.satcmd): D 1.31-1.36, slope 2.5-2.7, 0.5-1.4 holes — the outlines are the 5-km map's
  coverage thresholded; the morphology moves every number the right way but little (holes +20-50%, objects +25-40%).
  Tried: the ~20-km fraction alone (inflated the coverage: fewer holes), a flatter threshold (x0.55: 231 registers
  for +15% holes). **Compare at one ground resolution** (`--scale`: the renders' clouds are half-res, the MODIS thumbnails
  sharp; at full res every render reads D ~1.33 whatever the layers): at ~1 km/px real D 1.47-1.73, slope 1.5-2.35,
  holes 1.9-9.8; ours (morph on) 1.37-1.42, 2.27-2.41, 0.6-1.3. Isolating layers changed nothing (low only = all);
  baking the coverage FRACTION into the weather cube and taking the far field's fraction from ~20 km alone (local branch
  `coverage_frac`, unmerged) changed nothing either. The remaining gap is 1-5 km structure, which the half-res march
  cannot resolve from orbit (the morphology's fine term is filtered to its mean there). The mid layer's sub-pixel haze
  takes the closed cells at its regime's area fraction (`uMid`).
- **Review 18 (2026-10-01): MEO flicker, imagery morphology, true coarse coverage.** (1) **Flicker** of scattered
  puffs from medium orbit: each visit's ray lands at another point of its 5-20 km texel (the R2 sub-texel jitter),
  and the far field's 2D reads were filtered to the footprint itself, so a puff near the threshold was hit or missed
  per visit. The far field reads at twice the footprint (`fpF`), stretched by 1/|dir.up| at grazing views
  (`gCv2Stretch`, set by the view march only), and the still history weight falls to 40% from 300 to 3000 km
  (CPU, `look2.w`). 8000 km: pixels with std > 8 levels 1.0% -> 0.31% (`tools/harness/flicker.py`,
  harness_runs/meo). The sparse vs full rate and the step size made no difference (sub-texel sampling, not steps).
  (2) **`cloud_morph.rgba8` is real imagery** (`tools/make_cloud_morph.py`; the procedural set is
  `make_cloud_morph_procedural.py`): four MODIS scenes from NASA GIBS (closed cells off Peru, open cells in the
  North Atlantic, streets off Japan, popcorn cumulus over the Amazon), cloud = the darkest linear channel minus
  the clear background (a local opening for the land scene), clear pixels ordered by blurred distance to cloud,
  Moisan's periodic component (seamless), locally standardised over ~30 km, clear stretches filled from another
  scene, rank-equalised. Credited in THIRD_PARTY_NOTICES.txt and the Attributions tab. (3) **The weather cube's
  coarse mips hold the brightness whose coverage is the TRUE mean coverage** of the fine texels
  (`cloud_v2_weather.comp`, N x N taps, `remap` push constants; mip 0, the types and precipitation unchanged;
  Coverage / clear / full are in the bake's settings hash). (4) **Far field:** the fraction 85% from mip 2
  (`wCoarse`, was 25%), the 5-km map's zero contour no longer drops a far-field sample (`cov <= 0` early-out
  only when the 20-km fraction is 0 too), and the strength sets the optical THICKNESS (`d *= 0.02 + 0.98 e^2`,
  thin parts translucent). "Morphology from (m/px)" (`morph_far_footprint_m` 1400, slot 237, `morph.z` = log2) =
  where the far field is full (in from a quarter of it), "Morphology breakup" (`morph_breakup` 0, slot 238,
  `morph.w`, pulls the far fraction toward 1/2: measured little; left at 0). Cost +0.4 ms at 420 km, +0.65 ms at
  8000 km (cloud march, 1920x1009). **What the morphology can show is bounded by the map:** from 420 km the far
  field is already full (the half-res pixel is ~1 km at fov 60), and over most of a broken region the map's
  coverage, even averaged over 20-40 km, is near 0 or 1, so the imagery only shapes the edges and the inside
  thickness; its 1-3 km structure (rims, puffs) is below the half-res pixel. The limb from 20000 km shows
  alternating half-res columns in cloud (open; not the far-field reads, the march settings, the TAA or the rate).
  (5) **The FAR CLOUD LAYER** (`cloud_v2_far.comp`, the user's design: "from afar switch to the detail textures
  instead of running the march"). What resolution buys was measured first: an HQ photo at 2x (the march at window
  resolution), downsampled, read D 1.51 / slope 2.03 / 19 holes per 1000 km^2 against 1.31 / 2.58 / 1.7 — MODIS is
  1.47-1.73 / 1.5-2.35 / 1.9-9.8. Sharper far-field reads in the half-res march gave only 1.38 ("Far-field sharpness",
  `far_sharpness`, slot 238, `morph.w` = 2^-it; left at 0). The layer: per FULL-RES pixel, the ray meets a sphere
  1.5 km up; `cv2FarColumn` (clouds_v2.glsl, a hand-kept copy of cv2FieldLow's far branch: same flowed and warped
  weather, the 20-km fraction, the morphology) gives the strength e; the optical depth is the type's extinction x a
  thickness from e (presence ramps in from e = -0.15: the march's lobes put cloud a little outside e = 0) and it is
  lit as a two-stream slab (R = a tau / (1 + a tau), a = 0.75 (1 - g), g 0.85) by the march's own `sunColorAt` /
  `skyZenithAt` (copied), with relief from e's gradient (two more columns a footprint away), the Moon and city light
  by night; the mid and high layers are sampled inside their lenses (3 heights each, placed from their own cluster
  reads) and added over it by the ADDING method (a plain attenuator drew grey camouflage over the decks). Output: full-res
  RGBA16F (`cv2FarImg`), march set binding 21, sky set binding 29 (imageLoad; the sky set's storage images 3 of 4).
  `CloudParams.farBlend` (was v1's unread hgG; `SatelliteSim::cloudFarBlend()`, smoothstep of the eye's altitude between
  "Far cloud layer from (km)" 600 and "... full at (km)" 1500, slots 239-240): cloud_march.comp fades the march's
  clouds out by it (and drops their occlusion distance once not opaque), sat_sky.frag composites the layer behind the
  half-res composite (`cloudA += farBlend L T_march; cloudB *= mix(1, T_far, farBlend)`; the air in front splits at the
  layer's distance for its share), and at 1 the v2 march and resolve are not dispatched. "Far cloud layer sunlight" 3
  (matched to the march's brightness from 8000 km, p97 232 vs 234) / "sky light" 1 (slots 241-242, `cv2.farLight`).
  Measured: 8000 km cloud bucket 9.3 -> 1.4 ms, 2000 km 11.9 -> 3.1-4.0 ms; still-view flicker at 8000 km 1.0% (start
  of review 18) -> 0.04% of pixels past 8 levels, 20000 km 1.4% -> 0.27%. **Not in the layer:** Cb towers and the
  map-fed anvil shield (storms from far orbit are the low layer's deep types), cloud shadows on the ground, rain.
  **No scene-depth test:** the half-res depth read per full-res pixel alternated over land from orbit (a dot grid
  through the layer). The march's scattered small puffs at ~2000 km (low layer, independent of the morphology: the
  margin's lobes) are not reproduced; the cross-fade band (600-1500 km) matches.
- **Review 19 (2026-10-01): ONE cloud placement at every distance.** Climbing straight up over one place, review 18's
  clouds were REPLACED between ~35 and 450 km: the near field (the cells thresholded by the 5-km map) and the far field
  (the imagery thresholded to the ~20 km fraction) were two placements switched by the pixel footprint (`farK`). Measured
  with `tools/harness/climb.py` (docs/HARNESS.md "Climb consistency"): detail seen from 40 km correlated ~0 with any view
  from 450 km up. Now `cv2FieldLow`'s placement is the near field PLUS the imagery at a fixed share ("Imagery share",
  `imagery_share` 0.5, slot 235, `morph.y`: 0.3 x it x clamp(zMs) in field units, zMs = the Perlin / morphology mix of
  "Morphology (orbit)"), thresholded at 1 - max(cov, 0.5 x the ~20 km fraction), the same at every footprint; a wider
  footprint only filters it. Cells too small for the pixel become PARTIAL COVER (`presFar` = 0.5 + (field - thr) /
  (2.5 sigU), sigU the cell variance the mip averaged away), the sub-pixel haze's opacity — never a second placement.
  `farK`, the far thinning (`d *= 0.02 + 0.98 e^2`) and "Morphology from (m/px)" are gone (`morph.z` unused).
  `cv2FarColumn` computes the same field and fraction (`CV2Far.frac`, the far layer's optical depth x it). EVERY consumer
  binds the morphology now — `cloud_march.comp` 22 (the ground shadow: shadows fell under the old placement), beam
  occlusion 9, lightning / light volume 20 — or its clouds sit elsewhere. Climb (user snapshot, fov 40): r vs 40 km at
  1500 km -0.06 -> 0.72, vs 200 km at 2500 km 0.25 -> 0.57, worst step 0.49 -> 0.70; close-ups match review 18's;
  cost unchanged. Tried first and dropped: the imagery ALONE everywhere (consistent, but the map-driven cumulus masses
  became scattered puffs and stratocumulus decks flat pancakes — the near look the user liked). **Resolve:** a pixel
  with no cloud of its own reprojects at the nearest cloud distance of its 3x3 new samples (`dNear`): at infinity, in
  motion low over small puffs, its history came from where a puff had been and the parallax floor (50 km) called it
  still — every small cloud smeared into a streak and faded (snapshot 2: walk |diff| vs settled 5.2 -> 3.1).
  **The sea:** the sky reflection's march was gated on reflStr (Fresnel ~0 within ~34 deg of the nadir) and the foam and
  surf take reflColor as sky light, so at dusk a hard light-blue disc sat under the observer; the open ocean's water level
  decoded to 2e-5 m (a "lake"); reflected clouds come only from on-screen texels.
- **Review 20 (2026-10-01, the user's night snapshots over SoCal):** **dust is lit by city light and the moonless sky
  at night** (cloud_v2_march.comp fog/dust march; the fog had city light, the dust only the Moon): from inside its band
  (eye under ~1.5 km) a long horizontal path through it hid the city-lit sky and gave nothing back — a near-black band
  along every night horizon that vanished on climbing (horizon 7 -> 42 levels; dust off reads ~60), and where a far
  cloud's half-res silhouette hid the dust behind it, a pale blocky band (snapshot 1: lower contrast now, its blocks
  remain). The dust reads the night map at ~80 km (`mix(3, 4, fd)`), `kDustCityK` 1. **The resolve reads its history
  Catmull-Rom** (sky_taa.comp's 9-tap form) and **"still" is stricter**: the moving weight, the tight box and variance
  clipping ramp in from 0.05 / 0.05 / 0.1 px of parallax (were 0.25 / 0.3 / 0.5). Climbing at the player's Q speed,
  horizon clouds 30-100 km out move ~0.1 px a frame and kept the still weight and wide box, so depth errors built up
  into a soft vertical smear (snapshot 4, beside the Reflect beams — not the shafts: shafts off changed nothing):
  tstab rise 0.87 -> 0.65 (full rate 0.51 at +5.8 ms), review 15's scenarios 1.21 -> 1.18, still floor unchanged.
  The adaptive rate's parallax threshold is not the lever there (0.2-1 px identical: nothing passes it).
  **Review 20b — the "chunkiness":** (1) **the resolve also clamps history in plain RADIANCE** (the new samples' rgb
  box, same slack): the mean-colour space divides by opacity (floor 0.02), and inside the Reflect beams every clear
  pixel carries shaft light at ~0 opacity, i.e. x50 there — a cloud-edge history texel clamped up to it came out as
  bright blue-white 2x2 blocks along every cloud edge in motion (user snapshot, 5.4 km over the Anchorage site; rise
  >16 levels 1.0% -> 0.1%). (2) **The resolved distance is the 3x3 new samples' opacity-weighted mean** (`dW/dWs`),
  in motion and into the still history: a translucent haze's mean distance is a noisy per-ray estimate, in motion a
  sparse block took its one sample (the depth history is still-only, review 15), the sky pass split the air tens of km
  too near, and dark 2x2 blocks flickered along distant cloud edges (snapshot 2, 11 km; debugview cloudairsplit in a
  `path play record=` shows it — transmittance and radiance were smooth). Still, the same noise left a thin dark
  stair-stepped line there and blocks in the horizon band (snapshot 1). Still-view flicker (420 km limb, 2000 km
  nadir) identical; review 15's set 1.21 -> 1.18.
- **Review 21 (2026-10-01):** **Sunset ground shadows** (`cloudGroundShadowV2`): the low stretch takes as many steps as
  keep them <= 2.5 km (20-60, read at 1.2x the step, <= 3 km). At a 0.3 deg Sun the stretch is ~200 km and a 1-km cloud's
  height band holds the ray for ~190 km: at 20 steps of ~10 km each step that landed on a small cumulus drew its own disc,
  one cloud's shadow a chain of separate ellipses (user snapshot, 8 km at sunset). Now one long streak (a faint ladder
  remains on thin ones). +2.3 ms of cloud march in that view, nothing above ~17 deg of Sun. Tried: the step's whole length
  as the footprint (the scattered cumulus averaged away: no shadows) and per-frame jitter (needs the sky TAA, which is OFF
  below render scale 1 — the user runs 0.85). **The far cloud layer's light** ("Far cloud layer sunlight" 10, was 3;
  "sky light" 0.3, was 1): from orbit most of a cloud pixel is the haze in front of it, and at 3 the layer's own light was
  a third of the march's and its blue sky term dominated (debugview cloudrad, cloud cores: far R/B 0.76 vs march 1.43) —
  bluish clouds from ~1500 km. The orbit grade is not involved. At 10 / 0.3 its radiance matches the march at 1460 km and
  its image at 3000-8000 km (review 18's "3" was matched on p97 at 8000 km, where few pixels are cloud). Saved settings
  keep the old values. Also: partial cover is an area mix in the layer (share x full-depth light), not one thinner cloud
  (no visible change here). **Sea:** waves out to "Sea wave range (km)" (`ocean_wave_range_km` 100, slot 237, UBO
  `oceanWaveRangeM` = v1's unread marchSteps; was a fixed 3-8 km) and "Sea wave sharpness" (`ocean_wave_sharpness` 0.5,
  slot 243, `oceanWaveFootK` = v1's unread lightSteps: x the footprint the octaves are filtered at; 1 = review 14); +0.1 ms.
  The reflection no longer decays with distance (exp(-d / 40 km), from the first waves). The reflected clouds' screen fade
  lies just OUTSIDE the frame (review 19's fade inside it took every row's last 8% off the clouds at a horizon view:
  bright bands at both edges). Open: the dark line where an overcast's far underside meets the sea horizon (not the sea
  shading: oceansurf is uniform there). Note: a harness `set clouds_v2.dust_amount` changed the whole scene there (clouds
  and Sun gone) and `knockout +fog_layer` nothing — not understood.
- **Review 22 (2026-10-02, the user's notes + snapshots 1-9 = profile_log records 9-17, record N = snap N - 8;
  plan in `.plans/REVIEW_22_PLAN.md`):**
  - **Cumulus lobes taper with height** ("Cumulus lobes (bottom)" 0.2 / "(top)" 0.07, slots 244/245, keys
    `cumulus_lobes_bottom/_top`, UBO `farLight.zw`): on convective types they replace "Lobes (3D)" (form.x, same
    units) over smoothstep(0.2, 0.9, zeta) of the cell; the old 0.8 -> 1.25 rise with height is gone. The old look
    is ~0.26 / 0.40. Tops are now smooth domes over lumpy bases.
  - **Rain falls ~10 m/s** near (8.4 in far layers; was 8). It cannot follow the rain rate: a drop's offset is v x t
    with t up to 600 s, so any change of v moves every drop at once.
  - **Rain-only samples take the thin-ice path** (one sample per two coarse steps, no fine steps, light LOD): under
    the record-10 storm the cloud march was 22 ms, ~15.5 of it the shafts' fine steps; 31.3 -> 20.2 ms GPU total.
  - **Drops follow the rain volume**: cloud_v2_lightning.comp's workgroups 1-4 fill a 32 x 32 rain-rate map (40 m
    cells, +-640 m about the eye at its height, `cv2RainMap` + `cv2RainWgMax` after the flashes in the flash
    buffer; host offset `kCv2RainMapOffset`); each drop layer reads it at its own point (`rainMapAt`), so a shaft's
    drops appear in the far layers first. It replaced a cv2FieldLow at the eye PER PIXEL in cloud_march.comp
    (cloud bucket ~20 -> ~12 ms in the storm). The rain ambience reads the map's centre +-120 m on the host;
    `terrainFrame.w` is no longer written.
  - **Heavy rain under the dominant towers**: `gCv2RainCore` (set by cv2ColumnSigma, read by cv2FieldLow's rain
    branch; cv2Field calls them in that order — any other caller of cv2FieldLow sees 0) is a core under each
    anvil-reaching tower's base, 2.5x longer downwind; the shafts take max(map rate, core) at up to 2x density.
    The tower search now runs down to the ground while it rains (it stopped at 400 m). The curtains read 1.5 mips
    coarser with a softer threshold (the striped on/off pattern).
  - **The Sun behind the clouds toward it** (`CloudParams::sunCloudT`, was the unread `cloudsV2`): the lightning
    pass's thread 1 marches 400 km toward the Sun from the eye (below the shell top; 1 above it) into
    `cv2FlashPad2`; the host eases it (0.15 s, `sunCloudTEased`) into the UBO. It multiplies the Sun disc, glare,
    corona and lens flare (min with the pixel's cloudBlock), the Sun's bloom seed (`sunRefIntensity`), the direct
    in-scatter of air within ~50 km below 8-14 km AND within ~3 km of the eye's line to the Sun (sat_sky.frag; over
    the whole sky one cumulus in front of the Sun would darken everything), and the key light of RAIN samples within
    ~100 km (the rain phase's sharp forward lobe drew the Sun's disc in the shafts; with rain_amount 0 the glow is
    gone). A dim, soft glow remains.
    Bounding the key light of ALL cloud samples near the Sun's direction was tried and reverted: it punched a dark
    hole where the cloud toward the Sun is thin. Harness `state` -> `clouds_v2.sun_cloud_t`.
  - **No concentric rings round the nadir** (snap 8): the ray's start jitter now spans the COARSE interval (two
    steps); over one step the coarse lattice took half its phases and a thin deck seen from above was skipped at
    some, found at others. Test: a 30% change of "Step base" moved +-16-level bands before, 2 levels after
    (`harness_runs/r22_ringdiff*.png`). The fine restart's jitter is fract(1.618 j + 0.5). A per-interval sample
    position or a second blue-noise read flipped the march to 222 registers.
  - **"Fast-flight cloud LOD"** (slot 246, `fast_flight_lod`, default on; state `clouds_v2.fast_lod`): boost held
    and moving, or > 150 m a frame: <= 2 light steps, 60% of the iteration budget, a 1.5x base step. Only -0.5 ms
    climbing through the storm in the harness. Value 2 = whenever the eye moves (> 2 m a frame), boost or not (the
    harness holds no keys): boosting at record 10 (109 m, ~11 m a frame) cloud march 11.5 -> 9.8 ms but tstab boost
    1.80 -> 2.35 and the worst frame unchanged (~26 ms; `path play`'s `gpu_worst_frame_ms`: the spikes are the
    cloud march, 18-22 ms against a ~12 ms mean).
  - **"Foveated full rate (radius)"** (slot 247, `fovea_radius`, morph.z, default 0): an experiment — centre tiles
    at full rate while moving made strafing WORSE (tstab 1.60 -> 2.10) at the same cost. Left off.
  - **Far cloud layer**: the key light's reflectance is the two-stream slab's at the beam's incidence (`slabRmu`).
  - Looked at, not changed: snap 4 (record 12, 14 km) is the anvil lid's own horizon with the high layer seen past
    it and a taller storm beyond (debug view 7); snap 7 (record 15) shows no visible flicker in the harness (0.04%
    of pixels past 8 levels, on lit cloud bottoms under a 2-deg Sun); review 21's `set clouds_v2.dust_amount` "blanking"
    is the cloud history's reset (one noisy frame, then recovers; the settings hash includes it); snap 6 descending (record 14, tstab `fall`
    3.8 vs `rise` 1.8): history lag as the deck expands toward the eye, and the sky TAA's band as the eye's height
    changes.
- **Review 22b (2026-10-02, the user's follow-up):** **the Sun hidden by a storm, and the clouds in front of it
  dark:** at record 10 the composite's cloud transmittance toward the Sun was ~0.005 — the glow was the clouds' OWN
  forward-scattered light, the light march (a few km) not seeing the storm between them and a low Sun. The lightning
  pass's Sun march now writes a PROFILE (`cv2SunProf` in the flash buffer: its length + the optical depth from the eye
  at L (k/16)^2, k 0..16; the march includes cloud_lightning.glsl at binding 12): a cloud sample near the eye's line to
  the Sun takes exp(-(depth beyond its point)) on its key light, faded by its distance from the line (Gaussian, 2 km;
  8 km darkened a sunset-lit deck 3 km above the line). Clouds beyond the blocker keep their light (silver linings).
  The first cut overwrote the profile's last value with 8 after a full march (every cloud near the Sun line went
  dark): fill only after an early out. 128 registers. **Clouds through satellite meshes:** sat_sky.frag skips the cloud
  composite on a mesh pixel when the cloud's distance (|alpha|, km) is beyond the mesh — the march clamps only to the
  half-res depth (thin panels and truss missed) and the far layer has no depth test. **Go to:** 0.8-2.5 s
  (`followFlightDuration`), the view eases from where it looked to the satellite over the first third, and the arrival
  point is on the line from the satellite back to where the flight came from (`followFlightAlign`), so it settles in
  the facing it approached with. **Far sea:** the reflected clouds are averaged over 5 taps along the screen's vertical
  (spread by distance and roughness) and lean 30% more to the sky's own reflection past 3-30 km: a mirror finish to the
  horizon.
- **Review 23 (2026-10-02):** **Go to flies straight along the line of sight** when the Earth is not in the way
  (`followFlightLine`): the distance to the satellite closes by a constant ratio, along the start's line, onto the arrival
  point on that line; the arc (kept for a satellite behind the Earth) ended in a vertical climb to a point beside the
  satellite, where the facing came from a near-zero horizontal view and spun. The follow aim's facing keeps the previous
  one near the zenith / nadir (`setView`). **The aim is FREE on arrival**, held in the satellite's frame
  (`followBasis*`), so the satellite stays put on screen without the lock. **The sea's body** (kSeaBase, the crest and
  the subsurface term) is lit by the Sun + sky irradiance on the sea (`seaE`, as the foam), not by the cloud shadow alone:
  it kept its noon brightness up to the terminator and read cyan at sunset. **Sea grid:** a periodic 2D warp of the whole
  wave field (24 / 8 cells, `seaNoise2P`) and each octave's second component on a 45-degree lattice; the warp the
  octaves had was a shift along the diagonal only, and the ridges read as a square grid.
- **Review 24 (2026-10-02, the user's snapshots = profile_log records 2-7, harness_runs/r24_snaps.jsonl):** **Go to
  rides the OBSERVER'S line of sight both ways** (the parked ground observer to the satellite, re-aimed every frame;
  `followHomeEcef`, `followFlightS0/Delta0`): the distance changes geometrically, the arrival offset is on that line,
  and the way home keeps the satellite in view for ~60% before turning to the saved view, so a flare the observer
  sees stays in view in and out (benchmark: snap 3, Starlink Gen3 Direct-to-Cell n=7350). **Far-sea ripple**
  ("Far sea ripple", slot 248, `seaTune.x`): where the footprint filter has removed the wave octaves, a ripple normal
  tilted along the view, its cells ~3 along-view footprints (resolvable, so no aliasing; long thin bands on screen),
  on the waves' lattice with periods dividing kSeaCells — the far band was a mirror. "Sea warp" / "detail" (249-250,
  defaults 3.5 / 1.2: 4.5 stretched the ridges in places). **The far cloud layer was ~0.2 clearer than the march**
  (mean transmittance 0.76 vs 0.58 at nadir from 900 km): the march's 3D lobes put cloud past the 2D field's edge.
  "Far cloud layer coverage bias" (0.12, slot 252, `farTune.y`, field units) matches it (0.580 vs 0.576 nadir,
  0.43 vs 0.41 oblique at 668 km); "slant coverage" (251, apparent cover 1 - (1 - f)^(1 + k tan z)), "density" (253),
  "edge softness" (254). **Its key light went as mu0** (a flat slab) and went dark ~200 km before the terminator where
  the march's 3D tops stay lit: mu0^(1 / (1 + "low-Sun light")) (255, 0.5; far/march by strip across the terminator
  0.3-0.66 -> 0.92-1.23). The "Flat layers" sliders are v1's 2D layers: drawn only in reflections / probes, SKY_LITE
  and Potato, not the main view while the march runs. **Exposure from orbit**: the lit gate 0.08 -> 0.04 (review
  23's darker sea fell under it) and the lit target 0.42 -> 0.47 (snap 4: -1.37 -> -1.09 EV).
- **Review 24b (2026-10-02, records 8-9):** the far layer's defaults are the user's tuning (full at 8900 km, slant 0.91,
  bias 0.12, density 1.2, low-Sun 0.38, sunlight 4.1, sky 0.6). **Go to on moving satellites** (benchmarked at 1x,
  view-to-satellite angle per frame, harness_runs/r24b_goto2): the arrival takes the offset where the flight ends (the
  line rotated during the flight: a 1.4 deg jump on landing), and the way home stays on the satellite to the end and
  lands looking at it (turning back to the start's view swung the camera up to 55 deg while zooming out); 0.0 deg
  both ways. **The sea's waves fade with altitude over 20-60 km** (`altFadeW`; was 3-8 km, a pop of the wave texture
  and glitter path descending through ~7 km); the footprint filter does the rest. Mirrored features keep 3-8 km.
- **Review 25 (2026-10-02, records 10-12):** **a straight line across the sky in the clouds** (from under / inside a deck
  at 65 S) was review 22b's Sun-path darkening, applied only for cosSun > 0: with samples within ~2 km it was full on one
  side of the great circle 90 deg from the Sun and off on the other (debug view cloudrad stepped; transmittance and the
  air split were smooth). Behind the eye the distance to the eye's Sun RAY is the distance to the eye (continuous at
  90 deg); sat_sky.frag's near-air dimming doubled it there (also fixed). **Far-sea ripple cells are 5 along-view
  footprints** (3 drew value-noise blocks ~3 px tall: "pixelated") plus a component on the 45-deg lattice.
- **Review 27 (records 13-14):** the far-sea ripple's level blend drew RINGS round the observer (worst in motion): a level
  took a different offset as the incoming and the outgoing one (`seaRippleLevel(k)` is now one function of k), and the
  mix of two independent levels dipped to 0.71 contrast mid-way (renormalised). "Far sea ripple size (px)" (slot 256,
  `seaTune.w`, default 5): the cell in along-view footprints; lower = finer ripples, more shimmer.
- **Review 28 (record 15):** **the sea's hard line at the wave range** was the reflection's lost-slope roughness
  (`seaRough`), set only inside the range (blend < 0.99): past it the roughness fell from ~seaState / 2 to 0 in a pixel,
  wherever the range was. It is computed from the footprint everywhere now; "Sea wave range fade" (slot 257,
  `ocean_wave_range_fade` 0.4, UBO `cityLod2.z`) is the share of the range the waves fade over. **Mesh flares sit on
  their glint:** a meshed satellite's sprite (point, bloom, the point-like glare) is placed at the Sun's image in its
  dominant lobe (d = s - 2 (s.n) n from the camera, clamped to the lobe's half-side, weighted by its specular share;
  CPU double, `GpuMeshKeepList` entry w, octahedral), not its centre, so the flare no longer jumps when the mesh's own
  glints take the glare (12-36 px). Benchmark: Reflect Orbital #2700 from Fairbanks — the observer is 1.26 deg off its
  beam axis (it sees the mirror lobe's GGX tail), so up close the Sun's image is a FIXED point in the sky 1.26 deg from
  the satellite and the mirror grows past it (the "drift" is the optics; with time running it moves as the mirror
  tracks its site). The environment probes are not involved (the Sun glint is analytic; sharp reflections are per
  frame). The GPU-parity log's mismatches at ~170 m are the GPU's float satellite position (0.5 m = 0.17 deg there);
  0.02 mag at 6 km.
- **The low shape volume is read in a rotated frame (2026-10-04, `shapeRotA`, `anchorShapeA/StormA`):** a tiling 3D volume
  sliced by the sphere repeats wherever the local horizontal plane holds a short lattice direction of the tile (an axis,
  a face or body diagonal): the equator always holds z, and with the drift the meridians every 45 deg hold x, y or a
  diagonal - rows of the same puffs (user snapshot, 9.6 N 134.6 W, frame 0's clearance 0.007). The CPU keeps the identity
  where its clearance (min |up . v| over those directions, longer ones x1.3 / x1.6) is >= 0.05, else the clearest of
  three fixed rotations (harness `state` -> `clouds_v2.shape_frame`). A switch reshapes the lobes, never the
  placement. Tried: per-sample warps (Perlin, analytic sines, in place or not) all flipped cv2FieldLow to ~220
  registers (+5-9 ms); a cross-fade of two frames cost 0.7 ms idle. Only the low lobes and storm lobes use it; the
  detail erosion, cells, mid layer etc. still tile in the drifted ECEF frame.
- **`GpuCloudV2Params` mirrors `CloudV2Params`** (all vec4/mat4; offsetof asserts) — keep the order.
- Noise volumes are mip-mapped and read at the pixel footprint (`cv2Lod`). Lighting, shadow and beam
  samples pass detailAmt 0 (MEAN erosion) and the VIEW footprint; the coarse march passes -1 (none).
- All v2 screen images live in VK_IMAGE_LAYOUT_GENERAL (memory barriers only).
- **Settings layout (review 4, 2026-09-29):** the tabs are Constellations .. Photometry, then Clouds,
  **Weather** (storms, lightning, rain & snow, weather evolution, fog/dust/ice fog, god rays), **Atmosphere**
  (was "Aurora": scattering + the sky march's samples, airglow & zodiacal light, aurora), Terrain (relief;
  surface & lighting), **Night lights** (city lights, roads, sprites, city light on clouds, night sky light),
  Ocean, Beams (+ the beam light in clouds), Attributions. A tab's INDEX is persisted (`display.active_tab`)
  and indexes `hovTab[]`, so new tabs are APPENDED to `kSettingsTabNames` (Weather 12, Night lights 13,
  **Performance 14**) and the strip draws `kSettingsStrip`, grouped under headings (GENERAL: Display, Controls,
  Sound; SKY: Constellations, Photometry; RENDERING: the advanced tabs + Performance; ABOUT: Attributions — a
  negative entry is a heading) in its own vertical scroll view (an unclipped strip taller than the window
  stretched the body and spilled the content below the window). **Camera (3) has no button** since 2026-10-03:
  its two lines are the Controls tab's MOUSE section, and index 3 still opens Controls. `settingsTabIsAdvanced()`
  is the one "behind Show advanced settings" test (UI, toggle, harness). `settingsTabIndexByName("aurora")` still finds Atmosphere. Collapsible
  sections: `buildCloudSliderSections(..., base)` — each tab owns a range of `cloudSectionOpen` slots
  (Clouds 0-11, Weather 12-23, Atmosphere 24-29, Terrain 30-35; `kCloudSectionSlots` 48). Moving a slider
  between tabs changes nothing else: its slot and settings key stay.
- The Clouds tab's slider slots: `kCloudSliderSlots` (269 since 2026-10-05: 261-266 rain particles, 267-268 storm lightning / tendrils; 258 since review 28: 257 sea wave range fade; 257 since review 27: 256 far sea ripple size; 256 since review 24: 248-250 far sea ripple, sea warp x2, 251-255 far cloud layer tunables; 248 since review 22: 244-245 cumulus lobes, 246 fast-flight LOD, 247 fovea radius; 221; 212 = ground pattern range, 213-214 sea state / whitecaps, 215-218 aurora sheets, 219 orbit grade, 220 Moon size) sizes all four per-slider arrays and
  `cloudBufs` (212 since the 2026-09-29 reviews: 200-206 city sprites, twinkle, ground share, history
  moving, ice fog x2, sprite start; 207-209 Cb head lobes, drop distance, snow wind; 210 move speed (Controls tab), 211 erosion size); v2 uses 112-199 and (pass 10-12) 2, 7, 8, 9, 16, 17, 34, 50, 61, 71, 72-76. Still free from v1's deleted
  sliders: none (55-57 went to terrain v2's sky light / night sky light / close-up textures, 58 and 77 to the city street / major road lights, Terrain tab). (Slots 189-199: Cb fill, cumulus variation, sprites, adaptive rate, adaptive
  parallax, fog x3, dust x3.) Several `GpuCloudParams` fields are now unread (v1-only: marchSteps, lightSteps, hgG,
  shadowMaxDistM, maxRenderDistM, the AO/shadow knobs, cloudsV2) — a later compaction can reclaim them.

## Subsystem: Automation harness (2026-09-25)

User guide and command reference: **docs/HARNESS.md** (keep its table in step with
`harnessExec()` and `kHelp`). Architecture, for editing it:

- **Two halves.** `src/Harness.h/.cpp` is app-independent: argv options, the statement parser, the
  `Runner` (one command at a time across frames, `results.jsonl`, `summary.json`, the live inbox,
  the watchdog). `src/simulations/SatelliteSimHarness.cpp` is what each command does to the sim,
  plus `harnessStateJson()` (the capture sidecar) and the console's key handling.
- **Where it runs:** `harnessTick()` is the second statement of `buildUI` (after
  `beginCpuFrameTiming`), so a command's effect is in the frame it ran in and a `capture` after it
  records that frame. Time/observer/follow commands also re-run `updatePositions()` immediately so
  a `state` or sidecar in the same frame is not one frame stale.
- **Hooks it owns:** `Simulation::frameDt()` (fixed step; 0.5 s during `wait settle`, which also
  holds sim time), `wantsQuit()`, `onChar()`/`capturesKeyboard()` (the console; App leaves Esc to
  it), `Paths::setUserDataDirOverride()` (main.cpp points it at the run folder before `Log::init`),
  `wantsCleanScreenshot()` honouring `screenshotIncludeUI`, the crop/scale step in
  `finalizeScreenshot()`, `UIRenderer::requestLayoutDump()`.
- **Settings go through the settings.json path:** `loadSettings()` is now file read +
  `applySettingsJson(j, isPatch=false)`; `saveSettings()` is `buildSettingsJson()` + write. `set`
  applies a one-key patch (`isPatch=true`: no schema check, no intro re-arm, and no preset
  re-derivation unless the patch names the preset); `get` reads `buildSettingsJson()`. So every
  persisted setting is scriptable with no per-setting code — add a setting to those two functions
  and the harness has it.
- Camera paths (`path key/play`) and `overlay` text are harness state too (`harnessPath_`,
  `harnessOverlays_`); `path play` owns the clock (fixed 1/fps via `harnessFixedDtOverride_`, sim
  time set per frame) so a recording is uniform in time however slowly frames encode.
- **The player's camera lock is scriptable:** `track on|off` drives the same `startTrack()` /
  `stopTrack()` the selection panel's Track button does (nothing to do with `camera track <target>`,
  which is this harness's own aim-every-frame), and `state` reports it twice: `camera.tracking` and
  `selection.track`. `tools/harness/scripts/track.satcmd` is the worked example and `selftest.py`'s
  `track_toggle` is the regression test — the drawn evidence comes straight out of a `ui dump`
  (four `SelActBtn`, four `SelReticuleTick` only while locked).
- **`observer agl=` reads the GPU's ground back** (`terrainFrameBuf.y` -> host-mapped
  `terrainFrameReadBuf`, copied every frame after the depth pass): the CPU's `cpuTerrainHeightM` is
  an 18 km/px DEM copy, and until 2026-09-25 `agl` used it — the Big Sur golden view (agl=30) stood
  at 758 m over ground that is really at 342 m. `state`'s observer block used to add the terrain to
  the height offset a second time as well.
- `harnessRunner_` is null outside a harness run (and before the console's first use), and every
  hook is then a no-op. The first-run preset seed, intro, first-run notices, music and toasts are
  all suppressed in a harness run.
- **Launch spacing: the harness waits 30 s after an app exit before the next launch**
  (`tools/harness/launchgate.py`, used by `run.py` and `live.py start`). Back-to-back relaunches are
  the leading suspect for the machine freezes (docs/FREEZES.md, still unresolved; BIOS Gen3 did not
  fix them). For many batches use one `live.py` app, not a `run.py` loop. Freeze forensics
  (`blackbox.py`, `crashes.py`, `overnight.py`) are opt-in: `run.py --forensics`.
- **The loading screen runs in harness launches too** (its frames precede the script).
  `run.py --boot-capture` writes them to `captures/boot_NN.png`; see *Loading screen* below.

## HQ photo (review 13, 2026-09-30)

F8 (`KB_PHOTO`) or the sparkle-camera button beside the screenshot button in the bottom-left time bar:
`SatelliteSim::requestPhoto` pauses time, puts the clouds at full rate, scales the point and glare sizes by
the factor, and asks App for an offscreen target of "HQ photo resolution" (x window, `display.photo_scale`,
1-4, default 2) via `Simulation::photoScaleRequest()`. App then calls `VulkanContext::beginPhotoTarget` (an
image + depth + framebuffer APPENDED to swapImages / swapViews / framebuffers, swapExtent set to the photo
size — so every `swapImages[imgIdx]` user works with `imgIdx = photoIndex`) and `sim->onResize`, renders
"HQ photo settle frames" (`display.photo_frames`, 48) without acquiring, presenting or drawing the UI, and
the last one goes through the normal screenshot copy as `screenshots/satlight_hq_<time>.png`. Once it is read
back, `endPhoto` restores the settings and App the swapchain (a window resize waits for it). Every
footprint-based LOD (terrain, city lights) follows the finer pixels, so a photo shows more detail than the
window, not just more pixels. The bloom's radius is in its own texels and is NOT scaled (a tighter glow).
~2.4 s at 3200x1800 (1600x900 window, 48 frames). Harness: `photo <name> [scale=] [frames=]`.

## Cinematics (review 17, 2026-10-01)

`Cinematic.h/.cpp` (data: shots of `CineKey`s, `cineEval` — the Catmull-Rom path the harness used — JSON
`sat-light-sim-cinematic/1`), `SatelliteSimCinematic.cpp` (play / export / save / load), `buildCinematicWindow`
(SatelliteSimUI.cpp; the clapperboard button `TimeCineBtn` on the right HUD panel, `pixel--film.png`), harness `cine` + `path`
(docs/HARNESS.md "Cinematics"). **The harness's camera path IS the current shot** (`HarnessCamKey` = `CineKey`,
`cineKeys()`), so a path authored by a script shows in the window and the reverse. `cineTick` runs right after
`harnessTick` in buildUI; while `cineActive()` WASD/Q-E and mouse look are off and `timePaused` is held (sim time
set per frame from the shot). A cut applies the shot's stored look (`cineLookSettings`: clouds, clouds_v2,
photometry, constellations, planets, render settings — never observer/camera/time/window/keys/audio). Export
preview: `cineFixedDt_` = 1/fps (`frameDt`), one capture per frame, waiting for the COPY only. Export HQ: the photo
target stays up for the whole export (`photoScaleActive`, saved/restored like `requestPhoto`), each pose settled
"HQ photo settle frames" before its copy. The window's sliders hit-test their own laid-out track (the settings
slider helper is tied to the settings window's position). A FOLLOW shot (`CineShot::followSat`) rides with a satellite: its keys
interpolate `ox/oy/oz` (the camera's offset in the satellite's along/cross/radial frame) with follow mode's aim lock
(`harnessApplyCam` would end follow mode, so cineApplyAt sets the sim time and `followOffset` itself). A cut resets
`skyTaaHistValid` and `cv2HistoryValid`, and a preview export pre-rolls 16 frames at each shot's first pose. "Store
look" also stores the cloud map's DRIFT (`cloudDriftPhaseOffset`/`cloudDriftRate`, session state like a snapshot's
view): without it a reloaded storm shot framed clear sky. The application tour: `tools/harness/scripts/tour.satcmd`. **Motion blur** ("Motion blur (subframes)", harness `blur=N`): each
exported frame is N subframes over a 180-degree shutter (frameDt 1/(fps N); HQ subframes share the settle budget,
>= 4 each), summed in LINEAR light in `finalizeScreenshot` (`cineAccum_`; only the last subframe reaches the
encode). The settled temporal passes cannot blur (they reproject the camera's motion away), hence screen space.

**2026-10-03 revision** (the user: an HQ export "froze and crashed", follow inconsistent, text entry wanted):
- **The HQ "crash" was a ~20-minute export with no feedback.** The photo target is never presented, so the window
  showed one frozen frame for 451 frames x 48 settle frames at 4x the pixels (the log: 256 frames written, a clean
  exit). App now presents a **progress frame** every 0.25 s while the photo target is up (`App::photoProgressFrame`:
  wait for the photo frame, blit the photo image down into a swapchain image, the UI over it in `renderPassLoad`
  with `swapExtent` swapped to the window's for `ui.record`, present). `buildCineHud` (drawn even with the window
  closed or the HUD hidden) shows the run, frame n / N, the time left and Stop; **Esc stops** a playing or exporting
  cinematic (`capturesKeyboard` keeps it from quitting meanwhile). The window shows the HQ export's expected time
  before it starts (`cineEstimateHqS`: frames x settle x scale^2 / the current fps).
- **Follow shots keep their framing.** A follow key stores the view in the satellite's along/cross/radial frame
  (`CineKey::hasView`, `vx/vy/vz`, JSON `view_sat`) as well as the offset; playback sets it (`aimCameraAzEl` from
  the satellite frame) and `updateFollow`'s free view holds it. Keys used to keep only the offset and playback forced
  the aim lock, so any framing set after review 23's free aim on arrival came out different. A key without an offset
  (taken before Follow was switched on, `hasOffset` false or zero) plays from Go to's default spot behind the
  satellite, not its centre. "+ Key" / "Set" are refused (with the reason) on a follow shot while not following its
  satellite. Playback clears any Go to flight in progress; WASD in follow mode is off while a cinematic runs.
- **`cineEval` unwraps the azimuth** against the previous key (its comment said so; the code did not): a key at az
  240 after one at -120 spun the camera a full turn.
- **The window** (redesigned): Name field + Save (writes `<name>.json`; a new name saves a copy, `cineFile_` is the
  open file) + New; shot tabs, shot name, Duplicate / Delete; a **timeline** (click / drag scrubs, drag a key to
  retime it; built from fixed-width gaps, not floating elements, so it clips with the scroll view); each key's time
  is a text box that ripples the later keys; "+ Key at view" with a typed gap; the saved files with Load and a
  two-click Delete. Default height stays clear of the time bar.

## Bookmarks (2026-10-03)

`SatelliteSimBookmarks.cpp`; the bookmark button on the right HUD panel (`TimeBookmarkBtn`, `pixel--bookmark.png`) opens
the window (`bmChrome`, window id 6). A bookmark is the observer (`obsDir` as a double ECEF direction,
`obsHeightOffset`), the camera (az / el / fov), the sim time and the cloud map's **drift** (phase + rate — session
state, like a snapshot's `view`; without it a storm bookmark came back to clear sky). Stored in
`<user data>/bookmarks/bookmarks.json` (`sat-light-sim-bookmarks/1`) with `<id>.png` thumbnails beside it. Go
(`bookmarkGo`) stops follow / Track, sets everything and resets the temporal histories (a cut); a bookmark taken in
follow mode is a free camera at the camera's place.
- **Thumbnails** are the next CLEAN frame after Add / Update: `bookmarkTick` (every buildUI, before any layout)
  asks the screenshot path for a frame without the UI once nothing else is capturing (`bmCaptureFor_`), and
  `finalizeScreenshot` hands its pixels to `bookmarkCaptureThumb` (16:9 centre crop, box filter to 256x144) instead
  of writing a file. They live in ONE 2048x2048 R8G8B8A8_SRGB atlas (8 x 14 = `kBmMax` 112 cells), registered once
  with the UI — `UIRenderer` has only four external image slots — and each card's `UIImage` is its cell's
  sub-rect, inset half a texel. Saved thumbnails load lazily, four per frame, while the window is open.
- **Add and Delete are deferred to the next `bookmarkTick`** (`bmAddPending_`, `bmDeletePending_`): Clay keeps
  pointers into `bookmarks_` (the `UIImage`s, the meta strings) until the frame is recorded, so growing or
  shrinking the vector after the layout would leave them dangling. Go / Update / a rename change no address.
- The window: a name field + "Add current view", then cards in as many columns as fit (Clay has no wrapping rows:
  the column count comes from the list's last laid-out width) — thumbnail (click = Go), the name (a text field),
  when / where, Go / Update / Delete (two clicks). Harness: `bookmark add [name] | go <n> | update <n> |
  rename <n> <name> | delete <n> | list`, `ui open bookmarks`.

## Loading screen (2026-09-26)

`App::run()` creates the UI before `sim->init()`, with its pipeline built against
`ctx.renderPassBoot` (a third variant of the main pass, compatible with the same framebuffers), and
installs `Simulation::setBootStatus`. `SatelliteSim::init` calls `bootStatus("<step>")` beside each
`init:` breadcrumb, per texture and per satellite model (`bakeModelType`); each call appends a line,
logs `boot: <step> (<ms since launch>)` and presents one frame (`App::bootFrame`, `buildBootUI`).
Invariants:
- **`vkDeviceWaitIdle` before `ui.rebuildPipeline()`** hands the UI to `ctx.renderPass`: the last
  loading frame may still be executing with the old pipeline. Without the wait the first real frame's
  `vkQueueSubmit` failed with a lost device.
- Boot frames reuse drawFrame's command buffer, fence and semaphores and never touch the timestamp
  slots. The sim's key/char/cursor callbacks are ignored until init returns (Esc still closes). A
  resize during init is deferred to one `sim->onResize()` after it.
- `bootStatus()` presents a frame, so it may only be called from the main thread during init.
- Text fades on a perceptual curve (alpha = brightness^2.2): the UI blends linearly into an sRGB
  swapchain, so linear alpha steps look far brighter than intended. Font sizes never scale below
  16 px (the baked bitmap font breaks up smaller).
- `SATLIGHTSIM_BOOT_SCREEN=0` (`run.py --boot-screen off`) restores the old white-window launch.
- **The app keeps its own pipeline cache** (`VulkanContext::pipelineCache`, `pipeline_cache.bin` next to the exe, else
  the user data folder; loaded at device creation, saved after init and at exit, dropped past 256 MB; every
  `vkCreate*Pipelines` passes `ctx.pipelineCache`). The driver's Vulkan cache is capped (~512 MB on NVIDIA, one file
  shared by every build of the app): it filled on 2026-10-01 and stopped storing, and every launch recompiled every
  shader changed since — 30-40 s to "Starting" (pipelines 18 s, the clouds' bakes 4 s, then a frozen first frame).
  With it: 4.6 s on a warm cache. `VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR` is requested only during harness
  `shaders reload` (`ctx.capturePipelineStats`). Look at the `boot:` lines of satlight_log.txt when startup slows.
- **Music analysis cache key = a content hash** (kAnalysisVersion 2): the write time changed with every build's copy of
  the music, so every launch after a rebuild re-analysed all tracks (~10 s).

---

## Subsystem: UIRenderer / Clay

- `#define CLAY_IMPLEMENTATION` only in `UIRenderer.cpp`. All other files `#include "clay.h"` without it.
- `ui.input()` → `UIInput`: per-frame mouse/scroll/button state. `scrollY` positive = scroll up. `screenW/H` = window dims.
- `ui.mouseOverUI()` → **previous frame's** capture result. Read in `buildUI` to gate scene interaction.
- `ui.addMouseCaptureRect(x, y, w, h)` — call for every visible panel in `buildUI`.
- `Clay_Hovered()` only valid **inside** a `CLAY()` element body, not in the config struct.
- **Window titles** (`buildResizableWindow`): the title bar height follows `fs(16)`, and the title
  is `CLAY_TEXT_WRAP_NONE` in a horizontally clipped GROW box. A wrapped title used to spill below
  the fixed 36 px bar into the window body at large UI scales. Button labels that must stay on one
  line need the same `wrapMode`.
- **One-frame hover lag**: store `Clay_Hovered()` in member bools; use those bools for colors the next frame.
- `CLAY_STRING(x)` requires a **string literal**. For runtime strings: `Clay_String{ false, (int32_t)strlen(buf), buf }` with a **member variable** buffer (Clay stores raw pointers read after `buildUI` returns).
- **Clip rule**: never put `.clip` on a floating container that also has `backgroundColor` — SCISSOR_START fires before RECTANGLE, hiding the background.
- **Pointer-capture rule**: any `.floating` element with no explicit `pointerCaptureMode` defaults to `CLAY_POINTER_CAPTURE_MODE_CAPTURE` — Clay's root hit-test DFS stops dead the instant it finds the pointer inside that element, so nothing below it (in z-order) gets hover/click at all. Harmless for a normal panel (you want it to swallow clicks meant for it), but fatal for any floating element that is deliberately drawn *at* the pointer's own position every frame — the pointer is then *always* "inside" it, so it permanently blackholes every panel/button underneath. This shipped once as the gamepad virtual cursor dot (`buildUI`'s `VirtualCursor`): hover/click on every real button looked totally dead while the pad cursor sat still over it (the dot's own stale hitbox from last frame permanently overlapped the current test point), but worked in brief "blips" while the cursor was moving (the one-frame-stale dot briefly lagged behind the live position, leaving a gap for the real element underneath to get tested that frame) — a pattern that reads like a coordinate or deadzone bug but isn't. Any purely-visual floating element positioned at/following the pointer (cursor dots, drag ghosts, custom tooltips) MUST set `.pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH`.
- Scrollable containers: `.clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}` on the content div, and **call `ui.scrollbar(CLAY_ID("<that container>"))` right after the container closes** — a
  thin thumb along its right edge, sized from `Clay_GetScrollContainerData`. Without it a long panel
  looks like it simply ends at the window edge (the Settings tab bodies and the satellite window's
  section list both had no indication they continue below).

- **Text fields (2026-10-03, SatelliteSimUI.cpp "Text fields"):** `textField` (a string) and `numberField` /
  `numberFieldD` (a value shown as text that becomes a text box on click — every shared slider row's value, the
  Photometry rows, the volumes, the Cinematics window). One field at a time has the keyboard (`textEdit_`): Enter /
  Tab / a click elsewhere commits, Esc cancels, Ctrl+A/C/V; the field applies a commit when it is next drawn
  (`doneId`), so no callbacks. While one is focused `onKey` sends everything to it (`textEditKey`, before the
  console), `onChar` types into it, the polled movement keys (WASD, Q/E, zoom, follow mode's) are off and Esc does
  not quit. `textEditEndFrame` runs at the end of every `buildUI` path (a scope guard): a field not drawn this frame
  loses focus. Display strings live in `textFieldBufs_` (Clay keeps the pointers until record). Typed numbers are
  clamped to the slider's range. Harness: `ui click` / `ui type` / `ui key` / `expect`
  (`tools/harness/scripts/text_fields.satcmd`).
- **Clips nest (2026-10-03):** `UIRenderer::record` keeps a scissor stack — a clip inside a clipped scroll view is
  intersected with it and its end restores the parent's. SCISSOR_END used to reset to the full screen, so anything
  drawn after a nested clip (a text box, a horizontally clipped row) escaped its window's scroll view. `ui dump`'s
  clip boxes follow the same rule.
- **Never `return` or `break` inside a `CLAY(...)` block**: the macro is a for loop that closes the element at its
  end; leaving it early leaves the element open and corrupts the layout. Set a flag and act after the block.
- **UI kit (2026-10-03, SatelliteSimUI.cpp "UI kit"): build windows and settings rows from it.** `uiSection` (small
  caps over a 1-px rule — the Cinematics window's form), `uiKV` (a label column and a value column), `uiStatTile`
  (a label over a large value in a dark inset), `uiButton` (a 20-px pill; `on`, `enabled`, `grow`), `uiToggleRow`
  (label + On/Off pill, custom words: the knockouts read Skip/On) and `uiChoiceRow` (label + segmented pills;
  returns the picked index). Ids are `CLAY_SIDI(key, index)` — scripts click them as `Key:index`
  (`ui click ReplayIntroBtn:0`) — and the previous frame's hover lives in `uiHov_`, so a row needs no hover
  member. Formatted labels go through `uiKitStr` (a per-frame ring; Clay keeps the pointers). Used by the info,
  trace and Bookmarks windows and the Display, Performance, Controls, Sound, Constellations and Photometry tabs;
  the slider rows are still `buildCloudSliderRows`.
- **HUD text entry (2026-10-03):** the time bar's clock and the right panel's latitude, longitude and altitude are
  `inlineTextField`s (a readout until clicked; the box starts with an edit form — signed decimal degrees, the
  altitude in the HUD's unit). `setSimTimeFromText` takes "2036-11-22 02:06[:30]", ISO with T/Z, or "HH:MM[:SS]"
  for the current date — the WHOLE text must match one form, the day must exist in its month and the year be
  1800-2200 (a day was only checked against 31, so 2036-11-31 rolled over to December 1); `setLatLonFromText` takes a sign or N/S/E/W and "lat, lon" in either field;
  `setAltitudeFromText` takes km/mi (the HUD's unit) or a m/km/ft/mi suffix, MSL or AGL by the HUD's toggle. Text
  that does not parse shows an amber toast (`screenshotToastWarn`) and changes nothing. **The altitude readout was
  wrong above the ground until then**: it showed terrain + offset as MSL and the offset as AGL, but
  `obsHeightOffset` is already above SEA LEVEL (floored at the ground), so MSL = max(ground, offset) and AGL =
  max(0, offset - ground), the ground the GPU's when the depth pass runs. Harness `expect state.<path>`
  (`tools/harness/scripts/ui_windows.satcmd`).
- **HUD layout (2026-10-03):** the bottom-left panel is the clock, the time buttons and the picture buttons
  (Screenshot, HQ photo, Star trails); the bottom-right panel ends in the MENU buttons — Bookmarks, Cinematics,
  Settings (`menuBtn` in `buildRightHudPanel`: lit while the window is open). Their ids kept the `Time` prefix
  (`TimeCineBtn`, `TimeBookmarkBtn`) for scripts. The tutorial's last step ("Menus") outlines the three.

### Icon Atlas
- `ui.loadIcons(ctx, paths, count)` — loads PNGs, packs into RGBA GPU atlas, rebinds descriptor. Call once on first frame (lazy init). Store `VulkanContext*` in your sim.
- Icons: `.image = {.imageData = (void*)(intptr_t)iconIdx}`. Renderer samples the atlas UV range for that index.
- Shader `mode`: `0.0` = solid rect, `1.0` = text glyph, `2.0` = icon sprite. Binding 1 is always valid (1×1 white placeholder at init).
- **The PNGs are generated, not hand-drawn: `python tools/make_icons.py [name...]`** rewrites
  `assets/icons/ui/pixel--<name>.png` from the shapes declared in that file (all WHITE + alpha, 48×48,
  3× supersampled edges, or — the `PIXEL_STYLE` set — solid and hard-edged on a 24-px grid doubled, the look of
  the gear and the camera: new icons are FILLED silhouettes with detail punched out (`cut`), never thin outlines) and prints a 48 px preview plus the box-filtered **16 px** one — the size they
  are actually drawn at, which is the only preview worth judging. Edit a `build_*` shape list and
  re-run; do not retouch the PNGs by hand, or the next run silently drops the change. To add an icon:
  add the builder + the `ICONS` entry there, append its path in `buildUI`'s lazy-load block (the count
  is `sizeof(iconPaths)/sizeof(iconPaths[0])` — no separate number to bump), add a `kIcon*` constant —
  and re-check the preview at 16 px before wiring it in.
- **Which icons the sim actually loaded is recorded once at startup:** `buildUI`'s lazy-load block logs
  `UI: <n>/<count> icons loaded ('assets/icons/ui/...', cwd '<dir>')` into `satlight_log.txt`
  (`Log::path()`), and `loadIcons` returns the number that *loaded* (it used to return the number it
  was asked for, so a missing file was invisible apart from the magenta 1×1 it substitutes). Assets are
  CWD-relative and `main.cpp` chdirs to `Paths::exeDir()` first, so the icons always come from the
  directory the exe sits in — if a regenerated glyph "does not appear", read that line before anything
  else (and note the old process keeps its old atlas: rebuilding does not touch a running instance).

### Font atlas is a fixed-size bitmap, not resolution-independent
`loadFont()` bakes ASCII 32-126 once via `stbtt_BakeFontBitmap` at a single pixel height
(`font.bakedSize`) into a fixed `atlasW`x`atlasH` R8 atlas. Every requested `CLAY_TEXT_CONFIG`
`fontSize` (via `fs(base) = base * uiScale`, `SatelliteSim.h`) just scales that ONE baked bitmap by
`fontSize / bakedSize` (`pushText`'s `renderScale`) — there is no per-size re-rasterization, so any
requested size well above `bakedSize` visibly upscales/blocks, most noticeably on large one-off
text like the intro's title captions (`fs(48)`/`fs(34)`, which can request 68-96px at high
`uiScale`). Bumped `bakedSize` 32→48 (session follow-up) with `atlasW/H` scaled by the same
`(48/32)²` factor (512→768) to preserve `stbtt_BakeFontBitmap`'s packing headroom — it's "a very
crappy packing" (its own doc comment) that silently drops/omits glyphs past whatever fits, so
`loadFont()` now checks its return value and logs a warning if that ever happens again. This is a
mitigation, not a fix: text is still a raster upscale, just from a less-coarse source. True
resolution-independence at arbitrary `uiScale`/title sizes would need an SDF font atlas instead — a
separate, larger change (new bake step, new glyph metadata, a distance-based alpha threshold in
the text fragment shader) not undertaken here.

### MSVC C++20 Designated Initializer Ordering
MSVC requires designators in declaration order:
- `Clay_LayoutConfig`: `sizing` → `padding` → `childGap` → `childAlignment` → `layoutDirection`
- `Clay_ElementDeclaration`: `layout` → `backgroundColor` → `cornerRadius` → `aspectRatio` → `image` → `floating` → `custom` → `clip` → `border` → `userData`
- `Clay_FloatingElementConfig`: `offset` → `zIndex` → `pointerCaptureMode` → `attachTo`

### Manual Hit-Testing
Clay does not expose element positions post-layout. Compute absolute positions from constants that exactly match Clay sizing declarations. Wrap labels in `CLAY_SIZING_FIXED` containers so layout width matches hit-test math.

---

## Subsystem: Ambient sound (2026-09-25)

Location- and context-aware ambience on its own bus under the music (Settings → Sound →
"Ambience", `audio.ambience_vol`). Three layers, app-independent → sim:

| File | Role |
|---|---|
| `src/AmbientSynth.h/.cpp` | procedural voices as miniaudio data sources: `wind`, `surf`, `hum` (noise through resonances), `drone` (harmonic wavetable + phaser + whine + compressor cycle + soft status motifs), `pad` (the glare chorus: up to twelve hollow odd-harmonic voices placed on a ladder of chord/scale/tension roles from pitch-class masks, per-voice pitch warp, a bloom when a voice joins, an FDN reverb), `bass` (the beam-site pedal: a beating pair on 2× the root, its fifth, a sub, drive, a slow downward sag and throb), and the unused-by-default `saw` (the first beam sound, which read as an FPV drone), `chorus` (procedural VLF chorus / whistlers / sferics, flanged — replaced by the recorded `vlf_earth` loop), `beeps` (FSK bursts) and `disk` (seek clicks). `rain` and `thunder` (rolls triggered by the lightning flashes, see *Clouds v2*). Params are atomics set by the main thread, eased per 64-frame block on the audio thread (a `snap` param — a mask — is not eased); seeded xorshift, so a render is deterministic |
| `src/MusicAnalysis.h/.cpp` | the soundtrack's key: per track, the tuning and a tuning CURVE over time, chroma, key (Krumhansl-Kessler), pitch set, a chord timeline; analysed on a worker thread at startup and cached per track in `<user data>/music_analysis/` (see *Tonality* below) |
| `src/AudioSystem.h/.cpp` | the ambience group, voices (sample loop or synth) with per-voice gain, one-shots with pan, the music PLAYER (see below), and **offline mode** |
| `src/simulations/Ambience.h/.cpp` | the layer table: `assets/sound/ambience/ambience.json` → per-layer target gain from named drivers, eased over `fade_s`, voices created lazily, events scheduled |
| `src/simulations/SatelliteSimAmbience.cpp` | the sim's context drivers, computed each frame at the end of the planets block in `recordCompute` |
| `tools/make_ambience.py` | builds the CC0 sample FLACs (fetch, high-pass, crossfaded loop, loudness-normalise) and `CREDITS.txt` |
| `tools/sound_tool/` | `SoundTool` (EXCLUDE_FROM_ALL): `--analyze <tracks>` runs MusicAnalysis and prints key / tuning curve / chords; `--render <synth> --out x.wav name=v\|a:b\|t/v,...` renders one voice with keyframed params — levels and timbre without launching the app |

- **A layer is ramps, never switches.** `gain × group gain × Π when-ramps × max(any-alternatives)`,
  a ramp being `[a, b]` (smooth 0→1, reversed for a > b) or `[a, b, c, d]`. Every transition is a
  crossfade by construction. An unknown driver/param/synth kind is a LOAD error (logged to
  `satlight_log.txt`), not a silently muted layer.
- **Fades are LINEAR slews** (`fade_in_s` / `fade_out_s` per layer, × the Sound tab's global
  multipliers): silent to full in fade_in_s, back in fade_out_s, whatever the distance. The first cut
  eased exponentially — it never finished (a layer 4 s behind was still at −20 dB nine seconds
  later), so a jump from the ground to orbit dragged the crickets and birds up with it. Harness check:
  1.2 s after a jump to 400 km, crickets and ground wind are below 1%.
- **One key for every pitched voice.** A synth param written `{"root": k}` is k × the tonal root
  (which follows the music — see *Tonality*): the comms drone on 2× (110/220/330 Hz), the datacenter hum
  on 1× with its whine on 16× (880), their status motifs on just-intonation ratios (1, 9/8, 5/4, 3/2,
  5/3, 2) of 4× and 3×, the cabin and aurora hums on 1× and 2× — they overlap in LEO and stay
  consonant. The first cut's FSK beeps (2.4 kHz, squared-off) and disk clicks + fan noise read as
  brash and as audio glitches; they were replaced by the two drones.
- **Groups** (`"group"`: wind water nature city space machines) each have a gain on the Sound tab.
- **Rain (2026-09-28):** driver `rain` = the cloud field's rain rate at the eye (cloud_v2_march.comp ->
  `terrainFrame.w` -> `terrainFrameMapped[3]`, eased 2 s) — the value the on-screen streaks use; layer
  `rain` (group water) plays synth `rain`: a band-passed hiss, a low rumble for heavy rain and a Poisson
  stream of drop ticks (each its own resonance, level and pan), all scaled by `intensity`. Not yet
  loudness-calibrated against the -24 LUFS convention.
- **Glare is answered by the MUSIC while a track plays (2026-09-27, the user's design).** Tuning a
  synth to the soundtrack in real time never fit its rhythm and harmony, so the composer wrote an
  **upwell stem** per track (`assets/sound/music/<track>_upwell.mp3` — a separate layer, not a remix:
  its waveform correlates ~0 with the track). `setAudio` pairs each track with its stem (a
  `*_upwell.*` file is never a track itself, nor analysed); `AudioSystem::loadTrack` opens both and
  `startSynced()` starts them on ONE engine frame (`ma_sound_set_start_time_in_pcm_frames`, 50 ms
  ahead of the mixer live, the current engine time offline); pause/resume re-seeks the stem to the
  track's cursor and restarts both the same way, and `update()` re-seeks it if their read cursors ever
  drift past 50 ms. A stem longer than its track (BIOS and LEO Motif, ~5 s tails) rings out after the
  track ends (`upwellTail_`); a skip cuts both. Its gain is the table's `"music_upwell"` block: a
  piecewise map of `glare_n` (1 → 0, 4 → 0.4, 20 → 0.8, 100 → 1), slewed linearly up over 2 s and down
  over 6 s by `updateAmbience` → `AudioSystem::setUpwellGain`, under the music bus (so the music
  volume and altitude fade apply). Verified in `ambience_beams.satcmd`: 1.0 facing the Reflect ring,
  0 looking away, and in the offline render the track and its stem sit at the same sample offset.
- **The beam layers speak only BETWEEN tracks**, under the `music_gap` driver: 0 while a track plays,
  a triangle over the silent gap (0 at its start, 1 halfway, 0 at its end — a generous swell, never a
  cut when the song ends), 1 when no music is audible (player off or paused, music volume or the
  altitude fade below 3%), slewed 1/4 per second. Both beam layers carry `"music_gap": [0, 1]`.
- **The beam sound is two layers (2026-09-27 rework).** The first cut (`beam_swell`, a saw stack on a
  `beam_view` driver — beams near the camera's line of sight) read as an FPV drone, and flying through
  the atmosphere put you next to beams you were not looking at, so it sounded the same almost
  everywhere. Now:
  - **`beam_glare`** (synth `pad`) is SCREEN-SPACE: its drivers `glare_n` / `glare_sum` count the flares
    actually glaring on screen, with the glare sprites' own test (`b = log2(effectFlare)/2` past
    `glareThreshold`, in the view with a soft frame edge). Source: `sat_flare.comp`'s bright-flare list
    (`oceanGlintBuf`, every satellite at effectFlare ≥ 1 with its ENU direction), copied each frame into
    host-visible `glareReadBuf` at the end of `recordCompute`; `computeAmbienceContext` projects it and
    zeroes the count after reading, so a frame whose flare pass was skipped reads an empty list. Not
    occlusion-tested (the sprite tests that per pixel). Range: **0 to ~470** — facing the Reflect ring
    from a lit site hundreds of mirrors glare at once (0.06 facing away, 0 from 120 km). One flare = two
    hollow voices (the chord's fifth and octave on 8× the root) that bloom and fade; more flares add
    voices down a fixed ladder (root, third, the ninth against the octave, fifth below, seventh, root two
    octaves down, sixth, then the tension tones), so the chord deepens and crowds with seconds; the
    combined strength grows each voice's pitch warp and opens the filter.
  - **`beam_site_hum`** (synth `bass`) is NOT directional: `beam_site` sums converged beams (aim error
    against the same 10° the cloud clustering uses) with a soft proximity to their SITE — 1 each within
    15 km of the camera, 1/(1+(excess/25 km)²) beyond. ~180 on the Topaz site, ~10 from 120 km. It is the
    bottom of the sound: a pedal on the tonic that grows drive, sub, throb and a downward sag with it.
  Both are mapped with multi-point mods (`"in": [0, 1, 3, 10, ...]`), since a linear ramp saturated on
  the first hundred flares. A param that is BOTH a `params` entry and a `mod` target is overwritten by
  the root write every frame (`Ambience::update` applies mods, then root and tonal params).
- **Tonality: the ambience is in the music's key (2026-09-27, `MusicAnalysis`, `updateTonality`).**
  Kept for the pitched beds (LEO cabin, comms and datacenter drones) and the between-track beam pad;
  the glare during a track is the upwell stem, not a synth.
  Every playlist track is analysed once (`music::Library`, worker thread; SYNCHRONOUS in a harness run so
  a script hears the same key every time) and cached as `<user data>/music_analysis/<track>.analysis.json`
  keyed by file size + write time + `kAnalysisVersion` (the exe folder's cache is a read-only fallback,
  which is what a harness run finds warm) — a track dropped into `assets/sound/music` is analysed on its
  first launch. Mono 11025 Hz, 8192-point FFT (1.35 Hz bins), spectral peaks → **tuning** (the circular
  mean of each peak's offset from the semitone grid) and a **tuning curve** (8 s windows, 1 s steps),
  **chroma** with each frame's local tuning removed, **key** (Krumhansl-Kessler on chroma + bass/2), the
  **pitch set** (3-7 strongest pitch classes) and a **chord timeline** (3 s segments, triad / sus / power
  templates). ~1 s for all three tracks. gravity_wave: Ab (1 2 3 4 5, Absus4) with its piano's bend
  visible as a ±30-cent swing of ~50 s period in the curve; fuse: G minor (+6 cents, steady); leo_motif:
  C major (−19 cents). Chroma-template harmony is rough (sus4 and sus2 on different roots are the same
  notes), which is why a chord is only ever used as a SET of pitch classes.
  The runtime: while a track plays the root is its tonic at the curve's pitch where the music is (the
  ambience bends with gravity_wave), the scale its pitch set, the chord its chord at the playback
  position; in the gap between tracks the root glides from the last track's to the next one's tonic over
  `tonality.gap_glide` of the gap (masks switch halfway); the octave is chosen when the SOURCE changes
  (nearest the current root, then kept in `root_range_hz`), so a new track is at most a tritone's glide,
  and nothing moves faster than `slew_semitones_per_s`. Synth params `{"tonal": "scale"|"chord"|
  "tension"}` receive the masks, RELATIVE to the root; `tension` (b2 + #4 by default) is reached only by
  the pad's last voices — the "beautiful but deeply uncomfortable" part, over the music's own notes.
  Off (Sound tab "Key follows the music", `audio.ambience_root_follow_music`), or before the analysis is
  ready, the root is the Tonal root slider and the scale `tonality.fallback_scale` (Aeolian).
- **Drivers are the listener's, i.e. the CAMERA's** (follow mode: the camera, not the parked
  telescope): `alt_m agl_m ground_m` (GPU ground, like harness `state`), `lat_deg lon_deg`,
  `sun_el_deg`, `ocean_near/_wide` (`oceanMaskCpu`: a full-resolution 1-bit mask from the DEM —
  the 18 km/px `earthElevCpu` made every coastal town half ocean), `veg forest desert ice`
  (classified from a 2048×1024 copy of the DAY MAP, so the sound agrees with the ground as drawn;
  the map is a dry-season mosaic, the Great Plains read tan — "not desert, not ice" is the grassland
  test), `urban` (night lights, the dome's response curve), `beam` (the `groundBeams` Gaussians at
  the origin: ~1.8e6 in a lit spot's core), `glare_n` / `glare_sum` / `beam_site` (the beam sound —
  see above), `aurora` (oval band only), `wind` (value noise over
  ECEF direction and sim time — the jet stream and the alpine/desert/ice/sea winds), `cloud` (the 2D
  coverage map's value overhead: bilinear over the 1024×512 `earthCloudsCpu`, drifted by the same
  `cloudPhase` the surface overlay uses and eased over 1.5 s so a Go to cannot step it — `wind_ground`
  follows it, a full bed under cloud and a light breeze in clear air, so the wind is loud in some
  places and quiet in others), `time_scale`, `following`, `intro`, and per shell group
  `<g>_count` / `<g>_near_m`, and `speed_mps` / `eas`: the camera's ECEF speed and its
  equivalent airspeed `v·√(ρ/ρ0)` (8.5 km scale height), eased over 0.3 s. **No layer uses them:** a
  `wind_rush` layer on `eas` was removed at the user's request — it swamped every other layer
  whenever the camera moved and lingered — so don't re-add movement sound without asking. A jump of > 20 km in one
  frame (observer, Go to) counts as no motion, so a teleport never whooshes; time warp alone moves
  nothing in ECEF, and orbital speed in vacuum gives eas ≈ 0.
- **Shell proximity is closed form, not a roster search.** Walker density at latitude φ is
  `N / (2π²R² √(sin²i − sin²φ))`, RandomShell uniform over the band, count within hearing range
  `σ·π·(H² − dr²)`, nearest `√(dr² + (0.5/√σ)²)`. **A Disk is not one plane:** each SSO ring's
  inclination follows its own altitude, so across the AI disk's 1400 km the outer rings are tilted
  hundreds of km from the inner ones at that radius. The code takes the plane of the ring nearest
  the camera (its first satellite, in `initConstellation()`'s ring order). The first cut used one
  plane and read "0 satellites" from inside the disk; a second bug clamped the ring spacing to ≥ 1 km
  when the AI disk's is 0.7 km, which picked the wrong ring.
- **Offline mode (harness).** A muted harness run initialises the engine with NO device
  (`App::run` → `AudioSystem::init(offline)`): nothing is audible and nothing is mixed until
  `audio record` pulls the graph synchronously (`renderWav`, with a 0.25 s discarded pre-roll and
  per-1/60 s event ticks). Offline renders outrun miniaudio's streamer — the music came out with a
  0.25 s hole every second — so offline, streamed sounds are DECODED instead (music on the first
  recording that asks for it, loops at creation). In the live app loops STREAM: a voice is created
  the first time its layer becomes audible, on the main thread, and decoding a 36 s FLAC there is a
  hitch.
- **Levels.** At level 1, gain 1 and the default bus volume every synth kind measures ≈ −24 LUFS
  (`ambience_solos.satcmd`); samples are normalised to −24 LUFS. The table's gains then set the mix
  so each tour stop totals ≈ 10 dB under the music (≈ −35 against its ≈ −25 LUFS). No tonal
  "melody" content: hums are noise through resonances, beeps are data bursts.
- **Every synth output is high-passed** (wind 45 Hz, surf 55 Hz, disk 60 Hz, hum 32 Hz, saw 40 Hz, pad
  110 Hz, bass 28 Hz) and the
  sample beds per source in `make_ambience.py`: the first wind was a sub-bass wall (brown noise
  through a gentle band-pass) and its gusts were a linear swing that measured flat — gusts are a
  swing in dB now.
- **Samples are FLAC** (gapless loops; MP3 encoder padding breaks a loop seam, and miniaudio has no
  Vorbis). Loops at 32 kHz, 36-42 s. Sources are Freesound CC0 previews (128 kbps); a lossless
  original dropped into `build/ambience_cache/<id>.wav` is preferred on the next run.
  **Two are the user's own (FL Studio), not generated — don't normalise or regenerate them:**
  `vlf_earth.flac` (real whistlers, MEO: in over 1500-3000 km, out over 25000-50000 km) and
  `firmament.flac` (a pad on one long ramp from 1000 km to full at 40000 km, 8 s fade-in). Both are
  44.1 kHz, 55.2 s — the length of `leo_motif.mp3`, the music track written alongside them. They are
  not at −24 LUFS (vlf_earth −20, firmament −29): the table's gains (0.16, 0.42) compensate.
  `make_ambience.py` credits them in `CREDITS.txt` (`ORIGINAL_NOTE`).
  The jungle pair is one recordist's Amazonian set — `jungle_day` (676675) and `jungle_night` (868746,
  a night frog and toad chorus), both by felix.blume (2026-09-26); the first jungle day recording
  carried people talking, which is why the day bed and a night twin were re-sourced together.
- **Music player** (`AudioSystem`): the playlist is `assets/sound/music/` in the composer's order —
  `gravity_wave` ALWAYS first (the intro is cut to it; Replay Intro restarts it), then `fuse`,
  `leo_motif`, `BIOS` (`kOrder` in `setAudio`, case-blind) — then any other mp3/flac/wav in
  case-blind name order (`*_upwell.*` excluded: each is its track's stem — see the glare bullet above).
  **The intro belongs to Gravity Wave:** no upwell while `showIntro` (a replay drops it within 1 s),
  and the whole ambience bus at half (`AudioSystem::setAmbienceFade`, `ambIntroFade`, back over 2 s
  when the intro ends). The Sound
  tab's track line shows `+upwell N%` while a stem is up. Between tracks a silent gap (`musicGapS`, 30 s, Sound tab advanced) gives the ambience
  room; pause freezes the gap too. Sound tab: current track + position (or "Next: … in N s"), `<<`
  (restart if > 3 s in, else previous), `||`/`>`, `>>`. `loadTrack` must not call `stopMusic()` — that
  also switches the player off. **Music fades with altitude** (`updateAmbience` →
  `AudioSystem::setMusicFade`, a multiplier under the user's music volume): full to 1500 km, half at
  5000 km, silent from 35786 km (GEO/HEO), linear in log altitude, slewed 1/4 per second — the high
  orbits belong to `vlf_earth` / `firmament`. The intro tops out at 300 km, so it is unaffected.
- **Sound tab**: under "Ambience now", the key line (`tonalityLine()`: key, chord, root, source).
  **Advanced**: "Key follows the music" (on), music gap, fade in/out multipliers, tonal root (used only
  with the key not following), six group gains —
  `buildCloudSliderRows(..., marksPreset=false)` at slot idx 99-108 (the slot arrays are 112 now;
  idx 110/111 are the satellite ocean-glint pair below, rendered from the Ocean tab).
- Verification: docs/HARNESS.md "Ambient sound" — `audio state/record/expect/force/music/tonality`,
  `ambience_tour.satcmd` (self-checking), `ambience_beams.satcmd` (the glare drivers read back per
  camera heading and the site hum by distance, then the beam voices recorded), `ambience_tonality.satcmd`
  (the key per track and the glide through a gap — `audio music end` ends a track offline),
  `SoundTool --render/--analyze`, `imgtools.py audio` (spectrograms, LUFS). How it SOUNDS is the user's
  to judge.

---

## Subsystem: Controls / Keybinding Pipeline

**All interactive keys go through the `keybindings` vector.** The settings window and rebind UI are driven entirely from this vector — no extra wiring needed.

**Movement speed near the ground (review 6):** WASD's arc rate is capped at `moveSpeedPerHeight`
("Move speed (x height per s)", Controls tab, slot 210, `camera.move_speed_per_height`, default 1) x the
height above the ground per second, never below 3 m/s and never above the old 0.08 rad/s (~510 km/s, so
orbit is unchanged); boost and fine scale it the same way. A fixed 510 km/s crossed a cloud in a frame.
Q/E follow the same setting (review 7): `moveSpeedPerHeight` x (10 m/s + 0.5 x the height above the
ground), boost x10, fine x0.1 — it was 100 m/s + 0.5 x the height above SEA LEVEL.

### Gamepad layout 2 and the selection's focus (2026-10-03)

Default pad bindings: A select (centre pick) · B back · X pause · Y reverse · D-pad up/down faster/slower ·
LB/RB zoom · RS click reset zoom · LS click boost (held) · View toggle UI · Start virtual cursor; Move Fine
and Star Trails have no pad default (the sticks are analog). **`padContextButton`** runs ahead of the
bindings in `pollGamepad` and consumes a press it uses: with a satellite selected (and no cursor),
D-pad left/right move `padFocus` over its buttons (`SelActBtn` id % 4: Info, Go to, Trace pass, Track; Info
and Go to only with a geometry model, `selActionAvailMask`), A presses the focused one THROUGH the button's
own click path (`padActivatePending` -> `padActivateNow`, read by `buildSelActionButton`), B goes back one
layer (`padBack`: the 3D view / info / trace window, then Go to, then Track, then the selection). The ring and
a hint line under the panel show only while the pad was the last input. The windows themselves are still
cursor-driven. Saved bindings from layout 1 (B pause, D-pad time) keep their keys but drop their pad
buttons once (`controls.pad_layout`, `kPadLayoutVersion`). Harness `pad <button>` presses one without a
controller.

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

`SurfaceSpec`: `{AttitudeMode, specExp, weight, group, normal}` — oriented EITHER by a legacy
`AttitudeMode` (`group < 0`) OR mounted on a rigid attitude group (`group` + body-frame `normal`).

### Rigid attitude groups (lighting overhaul Phase 2, 2026-09-23)

Orientation is **data**, not an enum switch. A type has 1-2 `AttitudeGroup`s
(`kMaxAttitudeGroups`); each is a **two-vector law** — body `primaryAxis` points exactly at
`primaryTarget`, body `secondaryAxis` as close as possible at `secondaryTarget` (TRIAD, the STK/GMAT
align/constrain scheme) — plus an optional **1-DOF joint** about a body axis: `Track` (gimbal),
`EdgeOn` (knife-edge roll), `FixedAngle`, `FlareMitigationTilt` (the global slider). Targets
(`AttTarget`): nadir, zenith, sun, anti-sun, velocity, anti-velocity, orbit normal and its anti,
`SunReflectNadir`, `SunReflectGroundSite` (the TargetedReflector lock-window machinery, unchanged —
it now just produces `siteIdeal` for whichever group targets it). The `Tumble` law uses the
per-satellite tumble axis/rate/phase. All closed-form in sim time, so reversibility is intact.

- **Legacy modes are converted, not special-cased.** `resolveAttitude()` (`SatelliteSim.cpp`, top of
  file) runs for every type in `initConstellation()` and maps each legacy mode through
  `legacyAttitudeGroup()`'s table; the GPU only ever sees groups. The mapping reproduces every old
  `computeNormal()` result — verified numerically over 20k random geometries per mode: exact for
  all but the two joint modes (~1e-15). `Perpendicular` has no fixed-body equivalent in general
  (it depended on the primary's normal), becomes +Y of the primary's group, and is exact where it
  matters: every shipped/custom roster uses it only with weight 0, where it contributes nothing.
- **GPU form** (`GpuAttGroup`, `sat_orbit.comp` `groupNormal()`): every body vector is stored in the
  group's TRIAD coordinates (computed once per type on the CPU, in double —
  `attTriadCoords()`), so the shader builds only the world triad from the two target directions.
  Groups are read straight from the SSBO by index — never through a local copy of the array,
  which a dynamic index would spill.
- **Authoring**: `constellations.json` types may carry `"attitude_groups"` and surfaces
  `{"group": name|index, "normal": [x,y,z]}` (schema updated). Every launch writes
  `satellite_types_resolved.json` to the user data folder — each loaded type in explicit form,
  pasteable back and rendering identically. `data/custom/constellations_attitude_example.json`
  has a legacy Gen2, its explicit twin, and a gimbaled-wing V2 Mini the old enum could not express.
- Only the two-surface photometric model reads these normals until Phase 3 (primitives/materials).

### Geometry models (lighting overhaul Phase 3a, 2026-09-23 — `.plans/SAT_LIGHTING_PHASE3.md`)

A type may be `{"name", "model": "<id>"}` in `constellations.json`, loading
`<exe>/satellite_models/<id>.json` (source: `data/satellite_models/`, copied by the build and
packaged by `PackageRelease.cmake`). All model code is `src/simulations/SatModel.h/.cpp`, which
also now owns the attitude types (`AttTarget`/`AttLaw`/`JointMode`/`AttitudeGroup`/`GpuAttGroup`).

- **Kinematic tree.** Groups are a tree: a ROOT has the two-vector law; a CHILD has
  `"parent"` + `"hinge": {position, axis}` (in the parent's body frame) and is always the parent's
  frame rotated about the hinge by its joint. Parents precede children; `kMaxAttitudeGroups` = 4.
  **Per-component pivots (Phase 4f):** a component of a CHILD group may set `"pivot"` (relative
  to the group hinge, like `position`); the joint then turns it about the parallel axis through
  hinge + pivot. That is one joint at one angle on several axes, like the ISS's four beta gimbals
  in a single group. Normals, and so every lobe and magnitude, are unchanged. A posed *position*
  gains `satPivotOffset()` = (R_parent − R_group)·pivot, and every position consumer adds it:
  occlusion samples/occluders on the CPU and in `sat_orbit.comp` (`pivotOffset`; the pivot rides in
  `GpuSatOccluder`'s pad slots), the reference ray-casts, OBJ export, and the mesh renderer
  (`MeshComponents` binding 7, `instPivotOffset`). Any child group, including one with children of its
  own: the offset involves only the component's group and its parent (the ISS's Zvezda arrays pivot
  on the SARJ group, which carries the beta group).
  All vectors in a tree use the ROOT's triad coordinates; on the GPU `typeFrames()` builds up to 4
  world frames parents-first into fixed registers (`pickFrame` selects — no dynamically indexed
  local array). `evalGroupPoses()` is the CPU mirror (hand-kept in step with `groupFrame()`).
- **Geometry → lobes.** Components (`plane`/`box`/`cylinder`/`cone`/`sphere`, `position` relative
  to the group's hinge, `rotation_quat` or `rotation_deg` intrinsic XYZ) with materials (presets in
  `satMaterialPresets()` — estimates until 3c — or model-local, optionally extending a preset) are
  tessellated once, then merged into facet lobes: identical (group, material, normal) faces merge
  exactly; beyond `kSatLobeBudget` (48) same-group lobes merge agglomeratively with the lost normal
  spread folded into α². Curved facets carry an intrinsic spread ((Δφ)²/12, Ω/2π) so a faceted
  cylinder glints as a band, not N mirrors.
- **Reflectance** (`modelFlux()` in `sat_orbit.comp`): per lobe Lambert + GGX/Schlick/Smith (or
  Beckmann, per material: `"distribution": "beckmann"`, `GpuSatLobe::distribution`); the
  sun's disk is folded into every lobe as `SUN_ALPHA` = 0.0023, which reproduces a flat mirror's
  physical peak `F·A/Ω_sun` (verified: 1.50e4 vs 1.47e4 m² at normal incidence) — so model types
  have no `mirrorBoost`/`crossSection`/`specExp` hacks.
  **Earthshine** is the vector irradiance of the visible cap of a Lambertian Earth (albedo 0.3)
  whose every ground point carries its OWN Sun cosine (`earthshineExact()`, `SatPhotometry.cpp`:
  the ring integral is closed-form, one numerical integral over the cap radius), applied as a source
  of that irradiance tilted from nadir toward the Sun (the lit crescent is off to that side), with
  Earth's angular size folded into α. It is tabulated once — `earthshineLut()`, 64 cap half-angles ×
  128 Sun-zenith cosines of (ln E, tilt) — and both `sat_orbit.comp` (`earthshineAt()`) and the CPU
  evaluator (`earthshineLookup()`) read that same table, so parity holds (table vs exact: p95 0.004
  mag). **Until 2026-09-23 it was `0.3·(R/d)²·litFraction`**, which treats every lit point as if the
  Sun were overhead: 17× too bright with the Sun on the sub-satellite horizon and 60-15,000× at the
  95-110° zenith angles every twilight observation is made at. That bug was carrying the benchmark:
  nadir faces (a black visor underside, white antennas) were lit mostly by it. Legacy types keep
  their own `(1−R/d)/2 · illumFrac` term, deliberately, so they don't change. Output is a real apparent
  magnitude mapped into effectFlare through the existing anchor: `flare = K_FLUX·I/r²·brightnessScale`,
  `K_FLUX` = 9.979e10 (0.008 ↔ mag 6).
  **Earthshine is a broad source (2026-09-24, `SatEarthLight` in `SatModel.h`).** The single tilted
  direction above is exact only for a face that sees the whole cap; a face edge-on to it got nothing,
  though the cap really gives it ~30% of a nadir plate's irradiance at 550 km (the black, Earth-lit
  sides the user reported). Diffuse and transmitted light now use the irradiance of the whole cap on
  the face's plane: an order-4 SH fit of the cap's irradiance function (11 terms — symmetric about
  the Sun-nadir plane, and the cosine kernel has no l = 3), from the same closed-form ring integrals
  (`earthshineExact(..., sh)`), floored at the exact vector value: `E(n) = max(SH(n), E·(n·dir)₊)`.
  Specular keeps the tilted source. Tabulated with the vector table as ratios to E
  (`earthshineShLut()`, `earthShA/B/C` in `sat_orbit.comp`, `GpuEarthshineLut`); CPU
  `satLobeEarthIntensity()` / `evalSatLobesEarthPosed()`, GPU `lobeEarthIntensity()`; the mesh
  renderer reads the same fit per instance (`GpuMeshInstance::earthX/Z/ShA-C`, `instEarthDiffuse()`).
  Order 2 was tried first: 6-8% of E worst case (a twilight crescent on the limb is a sharp source);
  order 4 is ≤ 2.9% (`--selftest` gate 5%). `SatPhotResult::intensityEarth` is now ABSOLUTE (already
  × the irradiance). Benchmarks moved ≤ 0.05 mag.
- **Validation + shape check.** At load every model is compared against a brute-force per-triangle
  evaluation (logged p95/max |Δmag| over configurations within 5 mag of its median brightness), and
  its rest/sunlit poses are written to `<user data>/satellite_models_debug/<id>_{rest,sunlit}.obj`
  (+ `.mtl`, OBJ Y-up, zenith up). **`SatModelTool`** (`tools/sat_model_tool/`, EXCLUDE_FROM_ALL:
  `cmake --build build --target SatModelTool`) runs the same pipeline from the command line and
  prints the lobe table — the authoring loop, no app launch needed.
- **CPU photometric evaluator** (`SatPhotometry.h/.cpp`, benchmarking milestone M1 — plan and
  results log live on the "Satellite Brightness Benchmarking" design page). `evalSatPhotometry()`
  is the double-precision mirror of `processSatellite()` → `modelFlux()` for one satellite at one
  instant: orbit (`satOrbitStateAt`, same closed form as `satEciAt`), attitude (`evalGroupPoses`),
  posed lobes (`evalSatLobesPosed`), shadow cones, earthshine → physical magnitude, 1000-km
  magnitude, phase and off-specular angles, dominant lobe. It is what SatBench, the photometry
  readout and exports measure with — **keep it in step with `sat_orbit.comp`** like
  `evalGroupPoses`. `sunDirEciAt()`/`observerEciAt()` are the ONE copy of those formulas —
  `updatePositions()` calls them — and `SatelliteSim.cpp` static_asserts its orbital constants
  against `satphot::`. Ground-site (TargetedReflector) types take their `siteIdeal` from
  **`satGroundSiteIdeal()`** (SatPhotometry, 2026-09-24): the double-precision mirror of
  `sat_orbit.comp`'s ground-site block - lock window with the per-satellite hash offset, `pairScore`
  in the shader's integer hash and float, night-side + elevation eligibility, the nearest-site
  fallback, the rate-limited ease. It needs the targets and settings (`SatGroundSiteAim`, built by
  `SatelliteSim::groundSiteAim()` from the same floats as `reflectorTargetsECEFBuf`) and the
  satellite's roster index, which seeds its site preference. Traces carry all of it in their header
  (`ground_*` keys), so a replay needs no target file; the tool's self-tests use 60 fixed synthetic
  sites and check the settled beam lands on its site (< 0.01 deg). Every CPU consumer uses it: parity
  readout, traces, bulk export, the scene meshes and the tracked viewer - until then a mirror mesh
  was posed facing straight down. Legacy two-surface types aren't modelled (tuned display units, not
  a magnitude).
  `SatModelTool --selftest N` is its gate: known-value checks (Sun declination at the 2020
  solstice/equinox, orbit radius/velocity/inclination, zero off-specular/phase in constructed
  geometry, magnitude conventions) + posed lobes vs posed per-triangle brute force (must agree to
  < 1e-3 mag; configurations fainter than mag 20 are ignored as float noise).
  **The Earth rotation angle is GMST** (`earthRotationAngle()` = 280.46061837° + 360.98564736629°/day
  since J2000, the IAU 1982 linear part; 2026-10-03), so sim time is real UTC to ~1 s. Until then it
  was `kOmegaEarth·t` alone: self-consistent, but every place saw its sky ~5.3 h off real UTC. **Every
  ECI <-> ECEF rotation goes through that one function** (pc.gmst, gmstNow, the observer, the weather
  bake's Earth-fixed Sun, the harness) — never write `kOmegaEarth * t` again. `SatelliteSim::kGmstShiftS`
  (18901 s) added to an old time gives the old view; the start time and the cloud map's default phase
  moved by it, so the intro is unchanged.
- **Benchmark data** (`data/benchmarks/*.json`, format `sat-light-sim-benchmark/1`, loader
  `SatBenchmark.h/.cpp`, milestone M3): one published photometry dataset per file — citation,
  the paper's printed statistics each with a section/table `locator`, the `sampling` spec (shell,
  period, observer sites, stated and unstated constraints, censoring rule), individual
  `observations` when published (rows are arrays named by `columns`), and a `transcription` note.
  Kinds: `distribution`, `differential` (`references.test`/`baseline`). **A file is only trusted
  once `SatModelTool --benchmark <file>` reproduces its published values from its own rows** —
  `mallama2021_visorsat.json` (430 passes, App. A) reproduces n, the 7 censored rows, mean 7.218,
  median, σ and both phase fits (curves within 0.005 mag). `mallama2020a_original.json` is
  summary-only (that paper publishes no per-pass data); `visorsat_vs_original.json` is the 1.29 mag
  occlusion test. Summary-only files added 2026-09-24 for the parity models (Mallama et al.):
  `mallama2023_v2mini` (7.87, mitigated), `mallama2025_dtc` (6.47, held out - passes untuned),
  `mallama2020b_oneweb` (7.18), `mallama2026_amazon_leo` (6.81, operational mode at 630 km),
  `mallama2025_starlink_v15` and `mallama2025_guowang` (Table 1 of arXiv:2507.00107, whose text export
  shifts the count column one row - realigned by hand). Where a paper omits observers, split or
  period the file says so and assumes them. **`"gated": false`** (Guowang: orbit-raising satellites of
  unpublished hardware, model 1.6 mag faint) reports a miss without failing the run or the gate - a
  known residual, logged in `KNOWN_RESIDUALS.md`, not a way to hide a regression.
  Not copied next to the exe yet — only the tools read them.
- **SatBench runner** (`SatBench.h/.cpp` + `tools/sat_model_tool/bench_run.cpp`, milestone M5):
  `SatModelTool --run-benchmark <file> [--samples N] [--seed S] [--sensitivity] [--report-dir D]`
  simulates a paper's campaign with the benchmark's model (`satellite.model`): site weighted by its
  share of the observations, a time in the period with the Sun in the twilight window, then a
  random shell satellite above the elevation limit and fully sunlit, evaluated by
  `evalSatPhotometry()`, censored by the paper's rule at visual sites. Unstated assumptions (Sun
  window -18..-6 deg, min elevation 20 deg, period-start fallback) are `BenchRunConfig` fields
  echoed into the report; `--sensitivity` reruns under alternatives. Deterministic: mt19937_64 +
  bit-cast uniforms (never the implementation-defined std distributions) — same seed, identical
  report bar `run_utc`, on any platform. Reports (`sat-light-sim-benchrun/1`, gitignored
  `benchmark_runs/`) carry the git commit + dirty flag, FNV-1a hashes of the model and benchmark
  files, every sample, and the comparison rows (mean/median pass within 0.3 mag; sd, phase slope,
  and the **phase-matched mean** — samples reweighted to the observed phase histogram, since the
  observers' geometry selection is unpublished — are informational). Plots:
  `tools/benchmarks/plot_benchrun.py <report.json>`. Differential files run both references.
- **Provenance** (`sources` block in a model file, milestone M4): entries `{subject | subjects[],
  status: sourced|derived|estimate|calibrated, value, source}` — `SatModel::sources`, checked by
  `unexplainedModelParts()`. Every group, component and model-local material must be covered;
  `SatModelTool` prints the counts, every non-sourced entry, and any UNEXPLAINED part. A benchmark
  reference model is only accepted with zero unexplained parts. `calibrated` (M8) = fitted to a
  benchmark; its `source` names the benchmark, the metric and which checks stay held out.
- **Phase-curve benchmark + calibration (milestone M8, 2026-09-23).** Distribution reports now carry
  `phase_curve` (model vs observed mean m1000 per 10° phase bin) and the gated `phase_curve_rms`
  (bins with ≥ 10 observations, weighted by count, tolerance 0.3 mag) — the per-pass data's shape,
  not just its mean. `SatModelTool --set [<model>/]<material>.<field>=<value>` overrides a material
  after loading (echoed into the report), and `tools/benchmarks/scan_overrides.py` runs the
  VisorSat, V1.0 and differential benchmarks per override set and tabulates them. Findings, in
  order: (1) the earthshine bug above; (2) with it fixed, GGX beats Beckmann for the polished base
  (Beckmann made 70-110° 0.3-0.5 mag too faint, even though the base's α came from the
  Beckmann↔Phong equivalence); (3) two fitted values — `solar_cell` diffuse albedo 0.06 → 0.02
  (low-phase bins, where the array face dominates) and the VisorSat visor's size at Cole's fixed 23°
  cutoff (inset 0.55 m, gap 0.233 m; curve RMS 0.42 → 0.13). Results (5000 samples, seed 1):
  VisorSat phase-matched mean 7.220 vs 7.218, curve RMS 0.13, mean 6.97 vs 7.22; V1.0 (HELD OUT —
  its antennas are an untouched estimate) 5.926 vs 5.93; differential 1.04 vs 1.29. Remaining: the
  110-120° bin is 0.37 too bright (25 obs); model scatter 0.72 vs 0.85 observed (fixed attitude).
- **Selection panel (buildSelectedSatPanel) + the icon atlas (2026-09-24 UI pass).** The floating
  panel that tracks the selected satellite carries its name, its type and the magnitude readout, then
  one row of **icon-only** action buttons, all drawn by the single helper `buildSelActionButton`
  (which is also what the out-of-view chip uses): **Info** (pixel--info.png — a serif italic "i")
  opens the info window, **Go to** (pixel--eye.png) starts follow mode, **Trace pass**
  (pixel--trace.png) plots the pass, and **Track** (pixel--track.png, a sight reticle) locks the
  CAMERA onto it and re-aims every frame as it crosses the sky. Track is aim-only and moves nothing
  else: `updateTrack()` aims through `aimCameraAzEl` (so `obsFacing` stays the authority, and every
  look input — mouse deltas, the cinematic drift, the gamepad stick — is zeroed for the frame rather
  than left to fight it), while the observer stays where it is (WASD still walks) and the wheel's
  `fovYDeg` zoom is untouched, so the player can pull back and watch. The reticule answers the lock by
  tightening its corner brackets (radius 7 instead of 10) and adding four cardinal bars
  (`SelReticuleTick`: length 7, width 2, gap 3). Released by `select none`, `select planet`, any
  explicit aim (`camera az=`/`el=`, the harness's `camera look`/`camera track`, a scripted camera key)
  and by `startFollow`; `observer lat=`/`lon=` never releases it (the aim re-solves from the new spot).
  Unlike Info / Go to, Track needs no geometry model — any selected satellite can be tracked.
  **Each button's tooltip is its NAME, never a sentence** — the
  same rule the info window's title-bar Select / Go to icons follow — and the `on` state is the accent
  colour (the eye lights while following, the "i" while the info window is showing that satellite,
  the reticle while tracking).
  `kSelIconBtnMin` = 24 px targets, the info window's title-bar icon size; a planet selection gets no
  button row, and the Info / Go to buttons need the type to have a geometry model. Only the satellite
  ACTIONS are icon-only — the info window's CAMERA / RENDER / OBSERVER / CHECK rows are still text
  buttons. The orbit
  rows the panel used to list — altitude, inclination, RAAN, period, the flare-mitigation power readout —
  moved to the info window, as `viewerOrbitLine` under the image; the range/phase line moved to that
  window's OBSERVER box. **The GPU-parity Δmag line is gone from the UI entirely** (it was a
  development instrument in a panel the player reads): `updateSelectedPhotometry` still computes the
  check at the dispatch's own inputs and still logs a mismatch (throttled to 120 frames), it just has
  no readout line — so `selPhotLine` is one line now (`kSelPhotLines` = 1). Icons are packed into one
  atlas by `UIRenderer::loadIcons` in the order listed in `buildUI`'s lazy-load block
  (`assets/icons/ui/*.png`, index constants at the top of `SatelliteSimUI.cpp`); add a path there and
  bump the count when adding an icon.
- **Magnitude trace + CSV (milestone M9, 2026-09-23).** "Trace pass" in the selected-satellite panel
  (or its out-of-view corner chip — both are the icon-only `buildTraceButton`, pixel--trace.png, whose
  tooltip is just "Trace pass", since
  2026-09-24; see *Selection panel*
  above) runs the
  CPU evaluator over the selection's current pass — or its next one, within two orbits — at
  `kTraceSamples` = 400 points (`computeSelectedTrace()`), and
  plots it in its own window: apparent magnitude after extinction, above-atmosphere magnitude,
  phase angle on a second axis, whole-magnitude / phase / sim-clock tick labels (floating Clay text
  placed from the plot's last laid-out size), a moving "now" marker, and two rows of stat tiles (2026-10-03,
  `uiStatTile`): PASS — peak magnitude, its time, the pass length, max elevation (`tracePeakMag` etc., set by
  `computeSelectedTrace`) — and NOW — the clock, magnitude, elevation and phase evaluated per frame with the
  trace's own inputs (so the magnitude sits on the curve). The legend and both axis titles share one line; the
  toolbar is Retrace / Live / Export CSV (kit buttons; Retrace's tooltip carries one retrace's cost) and the
  export result. A status line (amber) says why there is no pass, or that the observer moved with Live off.
  **The window opens default 520x460 (min 340 tall) in the bottom-left, just above the time controls, so it
  coexists with the 3D view window in the top-right corner**; the title names the satellite as the selection
  panel does ("<constellation> #<n>"). **Live** (default on) retraces at up to `kTraceLiveHz` = 10 while the
  window is open, only when `traceStale()` says the result would change (observer moved, selection
  changed, pass over, extinction/tilt/occlusion changed). Tick
  labels are thinned to what fits the plot's pixel size (grid lines stay at every whole magnitude).
  When a satellite can't be traced (legacy type, ground-site aim, no pass
  within two orbits) the window says why instead of plotting. The plot is `UIPlot`, a Clay custom
  element that `UIRenderer::pushPlot()` draws as thickness-wide axis-aligned quads (no line
  pipeline). "Export CSV" writes `<user data>/traces/trace_<model>_<sat>_<sim time>.csv`, format
  `sat-light-sim-trace/1` (`SatTrace.h/.cpp`, shared with the tool): a header carrying every input
  (model id + file hash, lobe budget — `SatelliteType::lobeBudget`, roster-size dependent — occlusion,
  the orbit elements as `orbitElemsOf()` bakes them, observer, flare tilt, extinction k) and rows in
  the design page's export convention. `SatModelTool --replay-trace <csv> [--models-dir D]`
  re-evaluates every row and compares the written fields as strings (the M9 gate); `--selftest`
  runs a write/read/replay round trip per model. Occlusion in a trace is exact (no flux floor).
  **The selection panel's and corner chip's mouse-capture rects come from their real laid-out bounds**
  (`captureLaidOut()`, `Clay_GetElementData`), not size estimates: a click on the Trace button outside
  the estimate fell through to satellite picking, which deselected before the button ran — which is
  why low, faint satellites (shown as the wider out-of-view chip) could not be traced at first.
  **`APP_GIT_COMMIT` is stamped at CMake configure time** — reconfigure before a hand-off build, or
  traces and settings titles carry an old commit.
- **Bulk export (milestone M10, 2026-09-23).** Settings → Photometry → "BULK EXPORT": source (the
  selected satellite or a whole constellation), window (next 24 h / 7 days from the sim time),
  cadence (10/30/60 s). A worker thread (`startBulkExport()`, over snapshots of the type's lobes /
  occlusion and every source satellite's `orbitElemsOf()`) runs `runBulkExport()` (SatBench.cpp):
  every instant at the cadence with the Sun in SatBench's twilight window and the satellite ≥ 20°
  and fully sunlit — time-uniform sampling of visible satellites, which is what SatBench's random
  draws sample — evaluated by `benchEvalSample()`, **the same function SatBench's own draws now use**.
  Written to `<user data>/exports/samples_<source>_<sim time>.csv` by `writeBenchSamplesCsv()`, format
  `sat-light-sim-samples/1` — the one schema: every `--run-benchmark` distribution run also writes its
  samples there (`<report>.samples.csv`, header carrying the run's statistics).
  `SatModelTool --summarize-samples <csv>` reads either and, for a SatBench file, checks it
  reproduces the run's n/mean/median/sd (the M10 gate). `pass` numbers consecutive runs of visible
  instants per satellite; `satellite` is the roster index (-1 for SatBench's random draws).
  `photometryUnsupportedReason()` is the shared "why not" (legacy type; ground-site aim with no
  targets loaded).
- **Accuracy gate (milestone M11, 2026-09-23).** `cmake --build <dir> [--config Release] --target
  accuracy-gate` builds SatModelTool and runs `cmake/AccuracyGate.cmake` — **the only list** of
  checks: every `data/satellite_models/*.json` through `--selftest 400`, every `data/benchmarks/*.json`
  through `--benchmark` (transcription) and `--run-benchmark --samples 5000 --seed 1` (the logged
  configuration). Any non-zero exit fails the target. CI's Linux job runs it (`linux-accuracy-gate`
  build preset) between Build and Package, so a PR to main that moves a benchmark out of tolerance
  fails. ~40 s (23 models, 9 benchmarks, 2026-09-24). Verified identical on MSVC (Windows) and GCC 13 (Linux): the sampling is bit-for-bit
  deterministic and every reported metric matches to the printed precision. Adding a benchmark file
  or a model adds it to the gate automatically. **Accepted inaccuracies** (narrow margins, residual
  phase bins, fitted/estimated values, approximation errors) are logged in
  `data/benchmarks/KNOWN_RESIDUALS.md` — update it when a change moves one. The gate is deliberately
  NOT tightened to chase VisorSat (this is not a model optimizer); M11 runs in CI only on PRs/pushes to
  main, which are reserved for major releases.
- **Mesh renderer + model viewer (Phase 4b, 2026-09-23 — `.plans/SAT_RENDERER_PHASE4.md`).**
  `SatMeshRenderer` (`SatMeshRenderer.h/.cpp`) owns every model's render mesh in one vertex/index
  buffer, plus their materials and occluders. `buildSatRenderMesh()` (`SatMesh.h/.cpp`) is a
  separate tessellation from the photometric one: finer, smooth normals on revolved/sphere
  primitives, UVs in metres, and the same rest pose and body frame, so `evalGroupPoses()` poses it
  and `buildSatOcclusion()`'s occluders line up. `SatelliteType::model` keeps the loaded `SatModel`
  for it. Shaders: `sat_mesh.vert/.frag`, `sat_mesh_bg.*`, `include/sat_mesh_common.glsl`
  (descriptor mirror of `GpuMeshFrame`/`GpuMeshInstance`/`GpuSatMeshMaterial`/`GpuSatMeshOccluder`),
  and `include/earth_env.glsl`.
  - **Units: "π × radiance per unit solar irradiance"**, terrain's convention (a sunlit white
    Lambertian face = albedo × cos). The BRDF is the photometry's GGX/Beckmann · Schlick · Smith
    with `SUN_ALPHA` folded in, so a render's pixel sum reproduces the lobe model.
  - **Lighting:** the sun × litFactor, shadowed per pixel by the model's own primitives (the
    photometry's rule: a primitive never shadows its own surface); earthshine diffuse from the whole
    lit cap (`SatEarthLight`, per instance on the CPU) — or, with an environment probe, the probe's
    SH irradiance; moonlight at `moonGain` (terrain's scale).
  - **Environment probes = the full renderer's reflections and ambient (2026-09-24,
    `SatEnvProbes.h/.cpp`).** `sat_sky.frag` compiled with `-DSKY_ENV` (`sat_sky_env.frag.spv`)
    renders the sky from a SATELLITE's position (`pc.obsECEFDir` = its direction, w = its altitude):
    everything screen-space is cut (half-res cloud / depth / mesh targets, the sky-glow bins, ocean
    glints, beam ground spots), the flat cloud layers draw at full weight, the aurora gets a 16-step
    march of its own (`envAurora`), the sun disc and lens flare are left out, the Milky Way and
    zodiacal bases are turned from the main observer's ENU frame (`CloudParams::envMainObsDir`), and
    the output is pre-exposure HDR. **The post-tonemap terms (Milky Way, zodiacal light, stars) follow
    the VIEWER's rules (2026-09-25):** they are divided back by the exposure of the view that shows the
    result and gated by that view's sun glare — the main view's (`skyExposure()`, `skyGlareEased`) for
    scene probes, the model viewer's (`viewerExposureNow`, `viewerGlareEased`) for slot 0 and its
    background — carried in `pc.sunDirENU.w` = floor(exposure·100) + glare (the env fragment rebuilds
    sin(elevation) from `.z`; `envSkyPC(..., viewExposure, glareVis)`), and the Milky Way's
    sun-proximity dimming now applies in them too. They used the probe position's exposure and no
    glare, so a mirror showed the full Milky Way beside a Sun that had dimmed it everywhere else.
    **Stars** (2026-09-25): SKY_ENV draws the catalogue itself (`envStars`) — the main view's stars
    are point sprites no probe or mirror ever saw. `createStarEnvBuffer` bins it on a 6×32² ECI cube
    grid (skyDescSet binding 24; each star listed in every cell its widest PSF reaches, so a lookup
    reads one cell); each star is the point model's display flux (`point_style.glsl`, the header is
    rewritten by `uploadPointStyle`) as a Gaussian of the main view's sigma in THIS render's pixels,
    carrying its on-screen energy — sharp in the viewer and in mirrors, averaged away in a probe texel.
    A probe is
    six faces of that around one position (`faceCamToWorld`, checked against Vulkan's cube-face
    rule at init), a box mip chain, and an order-2 SH irradiance (`env_probe_sh.comp`, 1024
    Fibonacci directions, read from the 16² level). Face size per slot: the viewer's slot 0 is 512²
    (0.18°/texel, re-rendered ONE FACE PER FRAME, all six only for a new satellite — two a frame while
    it is > 20 km from where the cube was made; at 5 min/s "all six past 20 km" meant every frame),
    scene slots 256²; 128² read as pixelated in mirrors. `sat_mesh.frag` takes the texel size and
    last useful mip from the bound cube (`textureSize`/`textureQueryLevels`), reflects the probe (a
    mip whose texel spans ~2α) and
    takes its SH as diffuse light; the photometric earthshine still feeds the bloom normalisation.
    Each probe is its own cube image and descriptor set (pipeline set 1, bound per draw — no
    cube-array feature). Slot 0 is the model viewer's; scene instances share slots 1-7. **A probe
    belongs to the satellite it was made for and follows it** (`EnvProbeSlot::owner`, 2026-09-25): the
    owner keeps it however far it drifts; another instance shares the nearest probe within max(20 km, 2%
    of altitude) (and adopts one nobody claimed this frame — instances come largest first), else takes
    the least recently used slot. `kEnvRendersPerFrame` (1) re-renders per frame, new probes first, then
    the most drifted (> max(2 km, 0.2% alt), two faces at a time), then any older than 3 s. Until its own
    probe exists an instance BORROWS the nearest rendered one within 1000 km
    (`nearestRenderedProbe`); only with none does it fall back to `earth_env.glsl` + the photometric
    earthshine. Probes used to be keyed by position alone: at 5 min/s a satellite outran the reuse
    radius every frame, took a fresh slot, and every instance but the one rendered that frame dropped
    to the differently lit fallback — the ambient light of every mesh flickered. Off under Potato, the
    mesh knockout, or Settings → Photometry "Full-renderer reflections" (`envReflections`,
    `photometry.full_renderer_reflections`).
  - **Sharp mirror reflections (2026-09-25, `sharpReflections`, Photometry "Sharp mirror reflections",
    `photometry.sharp_mirror_reflections`).** A probe texel is 0.35° (0.18° in the viewer): a Reflect
    mirror filling the screen magnified each over dozens of pixels. For the mirror-smooth pixels
    (sharp share = 1 − smoothstep(0.004, 0.02, α): the `mirror` preset fully, an OSR radiator ~3/4) of
    up to `kMaxSharpReflInstances` (4) instances — those whose type `hasSharp`, largest rectangle on
    screen first — `sat_mesh.frag`'s scene variant writes a reflection G-buffer instead of a probe
    lookup (a 3rd scene attachment, RGBA32UI: octahedral direction, weight = Fresnel × tint × share ×
    fade, slot + 1; flag `GpuMeshInstance::earthShC.y`). `SatMeshRenderer::recordReflections` then
    draws `sat_sky.frag -DSKY_ENV -DSKY_REFL` (`sat_sky_refl.frag.spv`) once per instance inside its
    screen rectangle, from the instance's position (its slot + 1 in `pc.aspect`, which an env fragment
    never reads; skyDescSet binding 25 = the G-buffer), into an RGBA16F target, and
    `mesh_refl_add.comp` adds that into the mesh radiance, rescaling the alpha's photometric share (a
    reflection is not in the model the bloom is normalised by). Exact for a flat mirror at any zoom;
    costs a sky render of the mirror's area. The viewer still reflects its 512² probe (MSAA pass).
  - **Fallback reflections:** the reflected ray goes into `earthEnv()`, the Potato sky's analytic
    atmosphere, textured ground and flat cloud deck, rewritten in ECEF from any origin and scaled by
    `kEnvToScene` into pre-exposure units. The sun disc is not in it; the GGX sun lobe is the glint.
  - **The satellite window + the popped-out 3D view** (split + sections, 2026-09-24). Always a REAL
    satellite where it is now: the selection panel's **Info** chip (it was "View model"; the window
    leads with the orbit, brightness and observer readouts, so it reads as an info pane) opens
    `infoChrome`; "VIEW" on a constellation row picks that constellation's satellite highest in the
    observer's sky (`pickViewerSatellite`), SELECTS it (`selectSatellite`) and opens BOTH windows
    (`openModelViewer(..., popOut = true)`), so the viewer never shows an unreferenced satellite and a
    station is one click to find.
    - **Info window** (`buildInfoWindow`, default 470x~680 at the right edge, min 380x470): the title is
      the satellite's name as the selection panel gives it ("<constellation> #<n>"), amber while it is the
      selection, plus the **Select / Go to title-bar icons**
      (`buildViewTitleIcons`; both satellite windows carry them, each with its own hover pair, and
      `buildResizableWindow` skips the title drag when the click is on one — `viewTitleIconsHovered`).
      A **fixed 4:3 render band** sits at the top with the
      **view chips** floating over it (`buildViewChips`, anchored to the image element with
      `attachTo = ELEMENT_WITH_ID` so a resize cannot slide them off), a row of four **stat tiles**
      (MAGNITUDE as seen, ELEVATION, RANGE, PHASE; a phrase shortens to Dark / Below), then the scrollable
      collapsible **sections** — the Clouds tab's form (`infoSection`, `+`/`-` headers, because the font
      atlas is ASCII-only), every body a label / value grid (`uiKV`, 2026-10-03): SATELLITE
      (`viewerSatValue[4]`: type, model, size, mesh) and ORBIT (`viewerOrbitValue[5]`: altitude, inclination,
      RAAN, period, the flare-mitigation power) **open by default**, then BRIGHTNESS (`viewerPhotValue[5]`
      with `kViewerPhotLabels`: magnitude as seen, above the air, the extinction, at 1000 km, phase angle; +
      Trace pass), SKY POSITION (`viewerObsValue[4]`: visibility, elevation, azimuth, range), VIEW (the 3D
      view's camera / light / pose choices and the shadows, reflections, detail, glare, markers toggles +
      Reset view — the old CAMERA and RENDER sections) and CHECK (`viewerCheckValue[3]`: render, model,
      difference + `viewerCheckNote`; Run check), collapsed. `infoSectionOpen[]` is session state, like the
      Clouds sections'. The band does not scroll with the
      sections: it is re-drawn each frame from the same crop.
    - **The chips** (`buildViewChip`: the icon alone, tooltip = its name, on a translucent scrim so a
      white icon survives bright clouds) are the 3D VIEW's own controls, four of them: **Spin**
      (the free camera's idle rotation — a plain toggle now, so clicking it while lit stops it and
      "nothing lit" simply means the free camera; taking Observer clears the free camera so Spin reads
      off while it is active), **Observer** (the view from the ground observer's direction, which is
      what makes a ground flare's specular show on the model), **Studio** (LIGHTING, deliberately its
      own group: spin + live light is a legitimate combination) and **Maximize/Restore**. Select and Go
      to are NOT chips: they act on the subject rather than the view, so they live in the title bars.
    - **Pop-out** (`buildViewPopoutWindow`, winId 3, default 900x640 left of the info window, resizable):
      the same render, one chip row, nothing else. **One offscreen target serves both** — sized to
      whichever view is larger (`updateViewerView`) — and each view samples a centred sub-rect of its own
      aspect via `UIImage::u0..v1`, so a 4:3 band and a wide pop-out both show the frame unstretched
      (`viewerUvMini` / `viewerUvPop`). The small view keeps running while popped out.
    - **`updateViewerView` is the shared prologue** (called from buildUI before either window): it sizes
      the target and the background probe, registers the image, handles drag-to-orbit / scroll-to-zoom
      for EITHER view (hit-tested on the two image elements' last-frame boxes and gated on the chips'
      hover flags, or a chip click would also grab the camera), reads the photometric check back and
      refreshes the readouts at 10 Hz — all from the CPU evaluator on the VIEWED satellite.
      `recordModelViewer` runs when either window is open (it used to need the viewer open), so the
      photometric check still works with only the info window up. Both windows register mouse-capture
      rects, and the auto-grow-to-fit-column hack is gone — the sections scroll instead.
    - **The observer is the parked ground telescope, never the camera.** In follow mode `followObsEcef`
      is the camera itself (`updateFollow` sets it from the flight offset and even overwrites `obsDir`),
      so the readouts, the "you" marker and the Observer preset all use `followSavedObsDir` /
      `followSavedHeight` (saved on `startFollow`). That is also the fix for the glitchy marker line:
      with the camera as the marker's own endpoint, the line's clip and the dot's behind-camera test
      both degenerated. `showYouMarker` additionally hides the marker when the camera is within four
      model radii of it. A ground-site mirror still shows a solid orange line to its site.
    - **Markers are drawn OVER the mesh (2026-09-25, `sat_mesh_marker.frag`, `viewerMarkerPipe`):** a
      fullscreen pass after the model, depth-tested, no depth write. Each line pixel takes the line's own
      depth there (z/w is affine in screen space), so the model hides the part behind it and the part in
      front shows; the dots take depth 0 (always visible, unless the Earth hides them). Segments are
      clipped to w ≥ 1e-3 and |x|,|y| ≤ 1.5w in clip space before the divide — the first cut clipped
      only at w = 1e-4, and a line passing near or behind the camera projected to millions of pixels
      and flickered as the camera orbited close. "You" / "Target" labels: `recordModelViewer` projects
      the dots (`viewerMarkerLabels`), `buildViewerMarkerLabels` floats UI text beside them through the
      view's crop.
    - **Glare on the viewer's glints (2026-09-26, `viewerGlare`, RENDER "Glare", default on).** The main
      view's glare, drawn on the glints that make the flare the ground observer sees
      (`SatMeshRenderer::recordViewerGlare`, right after `recordViewer`): a half-res sun-only render from
      the viewer camera (`sat_mesh.frag` **mode 3** = the check's shading, writing the SPECULAR sunlight
      as L·d²), `viewer_glare_find.comp` (per texel effectFlare = L·d²·Ω/π × `viewerFlarePerI`, then
      glare_find.comp's 5×5 maxima), and `glare_mesh.vert/.frag` additive onto the resolved image.
      `viewerFlarePerI` (`updateViewerObserverInfo`) = K_FLUX·brightnessScale/r²·10^(−0.4·ext), the
      observer's, so at the Observer preset the glints carry their share of the ground flare, and the
      threshold is the main view's (glare from about mag 0.3). Live light + a tracked satellite only.
      Only reflections glare: the first cut gated on L > 2 (the most a diffuse face reaches), and a
      Sun-facing rough-glass array (L ≈ 1) never glared. Its glints carry the viewer camera's own range
      (`ViewerGlare::rangeM` = `viewerDist`), so they take the full proximity glare size (2026-09-26,
      above) — the viewer resolves a model from metres away. Harness: `viewer aim=observer|sun glare=`,
      `glints_last_frame`; `tools/harness/scripts/viewer_glare.satcmd`.
    - **Exposure = the sky's rule at the satellite** (`skyExposure()`'s curve on the Sun's elevation in
      its local sky, 2026-09-25): it was the day value whenever the satellite was lit, but a lit
      satellite over twilight is under the NIGHT exposure in the main view — the viewer read ~5x darker,
      as if it had no earthshine.
    Position, velocity and attitude come from
    `satOrbitStateAt` + `evalGroupPoses` in ECEF. With Live light its background is the SKY_ENV
    renderer at the viewer's own camera (`SatEnvProbes::recordViewerBg`, an HDR target
    `sat_mesh_bg.frag` samples and tonemaps) and its reflections come from probe slot 0; Studio light
    keeps `earth_env`. Markers: the observer (cyan dot + a dashed line from the satellite) and, for a
    ground-site mirror, its current site (orange). Camera presets: Free / From you (on the line to
    the observer: the side it shows you) / Toward you (behind it, 12° above the line, your marker
    beneath it). It follows the selection only when the selection CHANGES (`viewerLastSelected`).
  - **Diffuse transmission (Phase 4f)**, `SatMaterial::transmission` / `transmission_color`
    (presets `solar_cell_flex`, `solar_array_flex_back`: ISS-style arrays on a Kapton blanket).
    Light on the far side of a face leaves this side diffusely: `T/π·diffArea·(−n·s)₊(n·o)₊`. That
    term is in every lobe evaluator (CPU `lobeIntensity`, GPU `lobeIntensity`,
    `GpuSatLobe::transmission`, the brute-force references) and in `sat_mesh.frag`, for the sun and
    earthshine. The renderer tints it amber and, with the cell pattern, sends it only through the
    gaps between cells (`Surface::transM`, mean 1). A translucent lobe's occluder mask takes
    same-group occluders on BOTH sides (it is lit from behind). The first cut used the front-only
    mask, which was the ISS's largest occlusion error: its iROSAs shade the light the legacy
    blankets pass through (self-test p95 0.26 → 0.17).
  - **Render-only parts and open lattices (2026-09-24).** A component with `"render_only": true` is
    drawn and nothing else: no facets, lobes, occluder or `sources` entry — greebles cost only render
    triangles. `loadSatModel` moves them after every photometric component, so occluder i is still
    component i. A material's `"coverage"` (0..1, the `truss` preset: 0.3, pitch `"truss_pitch"`)
    makes an open lattice: facets count coverage × area, a closed primitive adds its inner faces at
    coverage·(1 − coverage), and the part never shadows (`satComponentOccludes`; its occluder is
    kept with `SatOccluder::blocks = false`, kind 0xFF in the renderer). `sat_mesh.frag` cuts the
    members out (`latticeCover`: bay grid + alternating diagonals, member width solved on the CPU for
    the same area fraction), kept per pixel against a fixed hash, so sub-pixel members at a distance
    become the right fraction of pixels with the far side's inner faces behind them. Quad UVs now
    start at a face corner, so bays and cell modules start at an edge.
  - **Procedural surface detail** (`SatMaterial::pattern`, JSON `"pattern"`: none / solar_cells / mli
    / panel_seams; -1 = from the preset — `solar_cell` → cells, `mli_foil` → crinkle). Visual only and
    **photometrically neutral**: each pattern scales albedo by a factor with area mean 1, leaves the
    specular alone, and is an exact box filter (`lineCover`/`gridCover`) so it keeps
    its mean at any distance. Each pattern has structure you can see with the whole model in frame:
    0.4 m array modules with gaps and a 0.25° tilt per module; MLI quilting seams and 12 cm crinkle.
    The first cut drew only cm-scale detail that faded out below a pixel, so it was invisible at
    normal framing. The viewer's "Detail" button toggles it.
  - **Photometric check** ("Check" in the viewer): a sun-only, single-sampled R32F render from the
    current direction at 60 model radii (`sat_mesh.frag` check mode writes L·d²), read back next frame
    and integrated as Σ L·d²·Ω/π, which is radiant intensity per unit irradiance. It is compared with
    `evalSatLobesPosed()` for the same pose, sun and direction, both as magnitude at 1000 km. With
    shadows on, the two differ by the occlusion sampling error (KNOWN_RESIDUALS); off, the difference
    is render tessellation plus smooth normals versus baked lobes. **Never give a `UIImage` element its own `backgroundColor`:** Clay emits CUSTOM
    before the element's own RECTANGLE, so the background covers the image. That was the first
    cut's all-black viewer. Put the backing colour on a parent.
    `recordModelViewer()` (end of `recordCompute`) places the model at its constellation's altitude
    above the observer and renders it offscreen: MSAA 4x, resolved into a swapchain-format image.
    That image is shown through **`UIImage`**, a Clay custom element (`UIRenderer::registerImage` /
    `updateImage`; each image gets its own descriptor set, drawn as a mode-2 quad). Lighting is
    Studio (sun at 35°) or Live (the sim's sun, including Earth's shadow); pose is Sunlit or Rest.
- **Meshes in the main view (Phase 4c/4d) + follow mode (4e).** Any model satellite whose mesh
  spans 1.5-3 px or more on screen is drawn by `SatMeshRenderer`'s **scene pass**, with no selection
  needed. It is recorded in `recordMeshScene()` just before `scene_depth.comp`.
  - **Choice (4d):** `sat_orbit.comp` writes each model satellite's on-screen diameter into the
    pre-photometry record (`GpuSatVisible::meshPx`, from `GpuSatType::meshRadius` and
    `GpuSatTypeHeader::meshPixelAngle`; the latter is 0 when meshes are off). `sat_flare.comp`
    lists every one past the CPU's nomination size (`GpuMeshKeepList::nominatePx`) into
    `GpuSatListHeader::meshCand[kMaxMeshCandidates = 1024]` (satellite, meshPx, final effectFlare,
    sprite size). The CPU sorts last frame's list by size and draws the largest
    `kMaxMeshInstances` (256): one frame of lag in *which* satellites, none in *where*. **The fade-in
    size adapts (`meshFadePx`, 2026-09-25):** eased toward the size of the 80%-of-the-cap'th largest (up
    in ~0.15 s, down in ~1 s) but never below the cap'th, so no more than the cap ever fade in and none
    is cut off part-way through its fade; the nomination size is 0.8× it, and it rises by 30% while the
    atomic counter says the list overflowed. Until then the first 64 appended were drawn — in
    atomic-append (random) order: in the AI ring ~250 satellites are mesh-sized at once, so which got a
    mesh changed every frame and neighbours flickered 50/50 between mesh and sprite. The instances are
    evaluated in parallel (`parallelFor`, SatelliteSim.cpp: `evalSatPhotometry` + `evalGroupPoses` per
    instance, a few µs each).
  - **Fades are decided on the CPU, this frame, in double** (mesh fades in over meshFadePx-2× it, 1.5-3
    px by default; the sprite fades out over meshFadePx-6.7× it, so the magnitude flare stays with the
    satellite while it resolves).
    The CPU hands the sprite weights to `sat_flare.comp` through `MeshKeepBuf` (descSet binding 12,
    `GpuMeshKeepList`). The first cut let the GPU fade a sprite the frame it nominated it, while the
    mesh came a frame later: a visible "flare, nothing, flare" gap. A kept sprite is drawn at 98% of
    its range so its own mesh doesn't depth-hide it.
  - **Picking:** meshed satellites are hit on their model's bounding circle (`meshDrawn`), ahead of
    sprites, since they have no sprite left to click.
  - **Followed satellite:** always drawn by the CPU, even in Earth's shadow where the GPU never lists
    it.
  - **Reverse-Z:** the scene projection is infinite reverse-Z (depth = 2 cm / distance, GREATER,
    clear 0), so meshes from 1 cm to 1000 km keep precision.
  - **Energy-matched bloom:** each instance's `bloomScale` is the bloom seed its sprite would have
    put down (flare_source: b(effectFlare) × 0.3926·s² over its point disc) per unit of rendered
    flux (Σ L·Ω·r²/π = I), times the share the sprite has given up (`spriteGone`), so sprite
    bloom plus mesh bloom stays constant. `mesh_bloom.frag` sums L·bloomScale per flare texel, reading the instance
    from the colour target's alpha (slot + 1). A flare keeps its full glow across the hand-off, and
    on a large model the glow sits on the glint. The first cut (1 texel per block over 2× white) was
    far too weak.
  - **Targets:** RGBA32F pre-exposure radiance plus R32F *true distance* (0 = none), full swap
    extent, GENERAL layout, always cleared.
  - **Depth pre-pass (2026-09-24):** each mesh is drawn twice — `sat_mesh_depth.frag.spv` (only the
    open-lattice discard) then `sat_mesh_scene.frag.spv` with depth EQUAL, depth writes off and
    `early_fragment_tests` (both `sat_mesh.frag` with `-DMESH_DEPTH_PASS` / `-DMESH_SCENE_PASS`;
    `sat_mesh.vert`'s `gl_Position` is `invariant`). The lattice discard had turned early-Z off, so every
    overlapping layer of a close-up station ran the full shader and its per-pixel occluder-loop shadow
    rays. Scene probes refresh TWO faces per frame (all six only when new); a satellite at 7.6 km/s
    crossed the 2 km drift threshold every few frames and re-rendered a whole cube each time.
  - **Consumers**, all through `imageLoad`, because `sat_sky.frag` is one below the 16
    sampled-image floor:
    - `scene_depth.comp` (binding 3) mins the mesh distance into the shared depth, so clouds and
      beams stop at it.
    - `sat_sky.frag` (bindings 22/23): a mesh nearer than terrain *is* the pixel's surface
      (`meshHit` → `tSurface = tMesh`). The atmosphere march stops there and attenuates it, every
      `tSurface` gate (sun/moon disc, Milky Way) hides behind it, and `gl_FragDepth` carries it.
    - `mesh_bloom.frag`, drawn inside the flare-source pass: over-white radiance × the sky's
      exposure seeds the bloom.
  - **Hand-off:** `SatFlarePC::meshSatIdx/meshSpriteKeep` (the old pad3/pad4) fade that satellite
    out of every sprite-side effect (point, bloom, sky-glow bins, ocean glint) as the mesh fades in.
  - **Positions** are CPU double (`evalSatPhotometry` for litFactor/earthshine, `evalGroupPoses`),
    camera-relative. The camera rotation is ENU(obsDir) × `SkyCamera`, the same basis the sky uses.
    Not drawn under Potato (`sat_sky_minimal.frag` has no composite) or knockout bit 2097152.
  - **Follow mode:** "Follow" beside "Trace pass". `followObsEcef` (double) =
    satellite + `followOffset` in its along/cross/radial frame. `obsDir`/`obsHeightOffset` are
    derived from it (the sky shaders read `max(ground, obsHeightOffset)` as altitude, so it is set
    to the altitude above sea level), and `updatePositions()` uses `followRadiusM`. WASD/Q-E move
    the offset (speed ∝ distance); RMB look unlocks the aim lock. Settings persist the ground
    observer, not the orbit. The panel's **Track** is the aim-only sibling: it leaves `followObsEcef`
    alone (the observer keeps walking), and follow mode releases the lock instead of fighting it.
  - **Reflections in the scene** come from the environment probes above (4c part 2, 2026-09-24).
- A model that fails to load logs why and falls back to the type's legacy fields. Examples:
  `starlink_v2_mini.json`, `hubble.json`, `iss.json` (the first parity model: 50 components; station
  root, TRRJ radiators edge-on, SARJ alpha, one beta group with four mast pivots; 8 legacy Kapton
  wings of two blankets each plus the six iROSAs installed by 2023; 514 exact lobes; the integrated
  truss is the `truss` lattice at coverage 0.5, 2.3 m bays, so it no longer shadows as a solid box), `tiangong.json`
  (T of three modules; the labs' two-axis wings as alpha about the labs' axis + beta with two
  pivots), `starship_depot.json` (9 x 60 m body of revolution in the `stainless_steel` preset with a
  body-mounted solar band), `spacex_ai_sat.json` (Starmind AI1, rebuilt twice 2026-09-24 to the project
  owner's reading of the AI1 spec sheet: nadir-pointing, a flat 10 x 3 x 0.6 m "door" bus that stacks
  PEZ-style with its 3 x 0.6 m end toward the Sun, two 10 m-long white radiators standing up and hanging
  down from its flat faces (20 m top to bottom, edge-on to the Sun), and two 70 m-span wings of three
  strips, each on ONE truss to a gimbal at its root, tracking the Sun about the span axis (then the
  flare-mitigation tilt, toward zenith, about +Y); 10 lobes, flown by a million; trusses, gimbals,
  spars and terminals are render-only), `reflect_orbital.json` (a 55 m square membrane mirror on `sun_reflect_ground_site`; its beams take
  the mirror's area and F0 from the lobes along the site-aimed axis — `bakeModelType` writes them into
  the legacy `crossSectionM2`/`mirrorFrac` the beam code reads, which a model type had left at 10 m²
  and 0, so no Reflect beam was drawn from the roster's move to the model until 2026-09-24),
  the Starlinks `starlink_v1_5` (gen-1 dielectric mirror film, a translucent 'lampshade' backsheet
  whose transmission is fitted to the Post-VisorSat phase function), `starlink_v2_mini` (rebuilt
  2026-09-24 to SpaceX's published mitigations: gen-2 film on the nadir face, black paint, opaque
  backsheet, arrays tracking the Sun but held within 90 - rho of zenith so their plane never dips
  below the Earth's limb), `starlink_v2_mini_dtc` (+ a 25 m2 nadir phased array) and `starlink_v3`,
  then `oneweb`, `amazon_leo`, `guowang`, seven commercial stations (`haven1`, `haven2`,
  `axiom_station`, `orbital_reef`, `ross`, `bharatiya_station`, `starlab` - low confidence, render-level
  sizes) and `debris_fragment` (tumbling) - the parity models; sources and estimates in each file,
  magnitudes in `KNOWN_RESIDUALS.md`. Presets added for them: `stainless_steel`,
  `dielectric_mirror_gen1/_gen2` (Beckmann: GGX's tail made the film forward-scatter grazing sunlight),
  `array_backsheet_dark` - and the M4 benchmark references `starlink_v1_0.json`
  (Mallama 2020a period: shark-fin, array edge-on to the Sun) and `starlink_visorsat.json`
  (Mallama 2021 period: array fixed 24 deg from vertical away from the Sun, radio-transparent
  visor sheet under the antennas) — both sourced from Cole 2021 (arXiv:2107.06026) and
  McDowell's size table; roster: the primary `data/constellations.json` (the working set). The visor's
  real shape is unpublished (derived from Cole's 23 deg full-shade constraint), and the visor only
  dims anything through occlusion — so the VisorSat differential needs Phase 3b.
- **Occlusion between parts (Phase 3b / benchmarking M7).**
  `buildSatOcclusion()` makes one occluder per component (plane, box, capped cylinder — a cone uses
  its larger radius, conservative — sphere; `kMaxOccluders` 64, a 64-bit mask per lobe) and
  `kDefaultLobeSamples` = 16 area-weighted sample points per COMPONENT of each lobe, up to
  `kMaxLobeSamples` = 64 per lobe (weighted k-means within each component over 16 sub-triangle
  centroids per source triangle, more on triangles over 32 m²; `bakeSatLobes(..., &lobeTris)`
  supplies the triangles). Until the ISS a lobe got 16 in all: its 16 array blankets share one lobe
  and got ~1 sample each, so any partial shadow on a blanket was all or nothing. A lobe's intensity is scaled by
  the weighted fraction of samples whose rays toward the light AND the observer both leave the
  satellite (earthshine: observer ray only), tested against every occluder posed at the live joint
  angles. Two rules keep it exact and cheap: **a primitive never occludes its own surface** (all
  are convex or flat — and without this a faceted cylinder's samples sit just inside the smooth
  cylinder and shadow themselves: Hubble read 3 mag too dim), and a same-group occluder counts only
  if part of it lies in front of the lobe (either side, for a translucent one; `occluderMask`, fixed
  at bake time). `--selftest` checks it against a ray-cast reference (25 points per triangle, one per
  ~0.5 m² on large ones, each component culled by its bounding sphere): p95 at 16 samples 0.14 on
  VisorSat (the visor's 8.5 cm gap), 0.17 on the ISS, 0.07 on V2 Mini, ≤ 0.02 elsewhere.
  **Coincident surfaces break the float GPU form:** two modules whose end caps touch, or a panel
  flush against a box, put samples exactly on a shadow edge, and float and double disagree there
  (the ISS read 0.54% of lobe evaluations different until its parts got 0.3-0.4 m berthing gaps).
  **GPU cost scales with lobes × samples on ONE thread per satellite**, so with occlusion on a
  station is far more expensive than a Starlink (the ISS: ~2300 samples at 48 lobes, more at the
  app's 256). Not measured yet. SatBench uses
  it by default (`--no-occlusion` = the M6 baseline).
  **GPU (2026-09-23):** `packSatOcclusionGpu()` converts it to `GpuSatOccluder` (80 B) +
  `GpuSatLobeSample` (32 B, point pre-nudged 1e-4 m) in ROOT TRIAD coordinates, like lobe normals,
  and writes each lobe's `sampleFirst`/`sampleCount`/`occluderMask`; `satOccluderBuf` /
  `satLobeSampleBuf` are bindings 9/10 of the `sat_orbit` set. The shader's group frames were
  rotation-only, so `typeOffsets()` rebuilds each child's translation from the frames and
  `GpuSatType::originT` (the rest hinge point) — the mirror of `evalGroupPoses()`' `pose.t`.
  `modelFlux()` computes the unoccluded total first, and only then — if `occlusionOn`, the type has
  occluders, and that total clears `occlusionFluxFloor` (mag `kOcclusionMagFloor` = 10; occlusion
  only dims) — subtracts each lobe's blocked share. Lobes facing away cost nothing; a lobe under
  1e-4 of the total is left unoccluded (≤ 0.005 mag, the one deliberate CPU/GPU difference). A
  sample whose observer ray is blocked stops its occluder loop. The parity readout applies the same
  on/floor gate. `--selftest` re-evaluates the packed GPU form in float the way the shader does
  (`selfTestOcclusionGpuForm`) against `satLobeVisibility`: 0 of 23,200 lobe evaluations differ
  across the five models, offsets within 3e-7 m. **In the app it is OPT-IN and off by default**
  (`satPartOcclusion`, Photometry tab "Satellite part occlusion", settings key
  `photometry.sat_part_occlusion`): measured 30.1 ms vs 2.8 ms orbit compute at 955k visible on the
  10M stress roster, for a subtle effect — no preset or first run may turn it on. Knockout bit
  1048576 can still force it off while it is on; both reach the shader as
  `GpuSatTypeHeader::occlusionOn` (`satOcclusionActive()`). SatBench keeps it on by default (physics
  validation, not an app setting). `data/custom/constellations_models_stress_10m.json`
  is the 10M all-model roster to profile it with. Still to do: calibration + reference models (3c),
  an inertial-pointing law (Hubble's attitude is an anti-sun stand-in).

### AttitudeMode values (legacy — converted to groups at load)
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
**`data/constellations.json` is the v1.1 roster ported to geometry models (2026-09-24)**: every v1.1
constellation with its v1.1 orbit and count (ISS and Tiangong at their current orbits) flying a model
type, ~1.38M satellites, plus Hubble, the 2020 V1.0 / VisorSat benchmark shells and a disabled
legacy Gen2 comparison. **Constellations ship enabled** - the user wants to see them (even the 1M AI
disk); the legacy comparison is the only disabled entry. The v1.1 legacy roster is preserved as
`data/custom/constellations_v1_1_default.json`. The build copies `data/constellations.json` over the exe-dir copy whenever the
exe relinks — which is why hand-pasted rosters there kept "reverting": edit the file in `data/`.

Types and constellations are loaded from `constellations.json` next to the exe. If the file is missing or malformed, `loadHardcoded()` provides the catalogue above as a fallback. The JSON schema is in `constellations.schema.json`.

### Adding a new satellite type
1. Add to `satTypes` in `constellations.json` (or `loadHardcoded()` as fallback) — either a legacy
   `attitude` per surface, or `attitude_groups` + surface `group`/`normal` for anything the legacy
   modes can't express (see "Rigid attitude groups" above)
2. No GPU struct changes needed; all fields map to existing `GpuSatType` members
3. Reference the new typeIdx in a constellation entry

---

## Subsystem: Orbital Mechanics / Constellations

### Orbit distributions
- **Walker** — `numPlanes × perPlane` satellites, evenly spaced RAAN, random phase per plane
- **RandomShell** — random RAAN, random incl in [0, c.incl], jittered altitude, random tumble axis
- **Disk** — concentric rings in a single orbital plane (incl + raan). `alignTerminator=true` derives incl/raan from sunDirECI at J2000 epoch and precesses RAAN at SSO rate (kSSOPrecRate = 2π/year)
- **Shells** (2026-10-06) — a filed shell table in ONE roster entry (`groups`: alt range, incl range or
  `sun_synchronous`, shells, planes per shell, satellites per plane; `ShellGroup`). Non-SSO groups are Walker
  shells (each shell's planes staggered); SSO groups take each shell's J2 inclination and spread their planes
  from the dusk node (LTAN 18:00), so 2 planes per shell = the 18:00 + 06:00 **X-ring** crossing at the
  equator. One entry rather than one per group because `enabledMask`/`highlightMask` (`SatOrbitPC`) are
  32-bit: the roster is at 29 entries, and an entry index >= 32 has no mask bit.
- **Clusters** (`cluster_size`; Walker, Disk, Shells): each plane's satellites fly in formation clusters,
  evenly spaced. 1 = off (Walker keeps random phases). `cluster_shape` "line": `cluster_spacing_km` apart along
  track; "ring": a regular polygon of radius `cluster_radius_km` in the ORBIT plane (along track x radial). A
  ring member's radius is the centre's + `SatOrbit::radialOffsetM`, but its mean motion is the centre's: R_sat
  and meanMot are separate fields on the GPU and in `satOrbitStateAt`, so the formation holds instead of
  drifting ~3 pi x the offset per orbit (it is station-kept, not a free Keplerian formation).
- **Starmind** (SpaceX orbital data centers) follows the 29 May 2026 letter to the FCC, Table 1
  (SAT-LOA-20260108-00016; transcribed in github.com/sdross0/orbital-datacenter-brightness ASSUMPTIONS.md
  rev. 2): SSO 565-585 km (10 shells), 707-744 (22), 967-1002 (22), 2 planes each; 30 deg at 550-568 km
  (10 shells, 26-32 deg), 686-718 (25), 946-978 (25), 30 planes x 333. The rows are per-group MAXIMA summing to
  1,198,120 against a 1,000,000 cap, so `per_plane` is scaled by 1e6 / 1,198,120 and rounded down to whole
  clusters (988,672 flown). Rings of 8, 200 m radius, in the orbit plane (the user's reading of SpaceX's
  visualization; SpaceX's text says "10-ish satellites" per cluster). Filed RAAN tolerance +-30 deg: modelled
  per plane via `raan_spread_deg` (10 in the roster; `raan_spread` even = golden-ratio sequence | random). It
  replaced a single 1M-satellite dawn-dusk Disk (575-1925 km, 2000 rings). **Why the spread matters:** with
  tight nodes every shell of a node family shares ONE plane, and shells 1.6-1.8 km apart with 2-km rings
  interpenetrate, sliding through each other at their different rates — 13,000 pairs of satellites from
  different clusters within 1 km at any instant (scratch rebuild of the same orbits, KD-tree, 12 instants); +-10
  even: 26; +-30 even: 12 (random +-30: 25, it clumps); the 30 deg shells (stagger only): 30.

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

All per-satellite orbital mechanics, attitude and reflectance computation runs on the GPU. The CPU only manages the small reflector targets buffer and triggers a rebake when needed.

**Lighting overhaul Phase 1 (2026-09-22, `.plans/SAT_LIGHTING_PLAN.md`).** At millions of
satellites the per-satellite cost was memory traffic, not math: ~304 B/satellite/frame (112 B orbit
read + an 80 B `GpuSatInput` written by `sat_orbit.comp` only to be read straight back by
`sat_flare.comp` + 32 B visible). Now 128 B (culled) to 160 B (lit: 64 B orbit + 32 B visible
write + 32 B read + 32 B in-place rewrite): the per-type fields moved out of every orbit record
into `satTypeBuf` (orbit 112 → 64 B), the reflectance model moved into `sat_orbit.comp`, and
`GpuSatInput` was deleted. `sat_flare.comp` is still a separate dispatch **on purpose**: its
photometry reads `beamGlowDome`, which `sat_orbit.comp` builds by `atomicMax` across every
TargetedReflector satellite, so it isn't complete until that whole dispatch is. Merging the two
would need a one-frame-stale (double-buffered) dome.

**Phase 1b — compact visible list (2026-09-23).** Measured at 9.88M satellites on an RTX 3070 Ti
after Phase 1: `sat_orbit` 1.7 ms (≈90% of the card's 608 GB/s — at the memory wall), but
`sat_flare` 1.27 ms + the satellite draws 0.7 ms were spent almost entirely on the ~90% of
satellites below the horizon. Now `sat_orbit.comp` **appends** only surviving satellites to a
compact list (`satVisibleBuf` records + `satVisibleIdxBuf` slot→satellite index), aggregated per
workgroup through shared memory (one global `atomicAdd` per 64 satellites — no subgroup ops, so
nothing to check on MoltenVK). It also maintains the indirect args in `satListBuf`
(`GpuSatListHeader`) by `atomicMax` on each workgroup's append end — slots are contiguous from 0,
so the largest end *is* the count, no separate args pass. `sat_flare.comp` is
`vkCmdDispatchIndirect`, and the satellite point, flare-source and trail draws are
`vkCmdDrawIndirect`, all scaling with the visible count.
Invariants:
- **Slot order is nondeterministic** (atomic append). Fine only because every satellite point
  pipeline blends additively (`ONE`/`ONE`, no depth write). Never key anything per-satellite off
  `gl_VertexIndex` in those shaders — it is a list slot, not a satellite.
- **`satVisibleBuf` is no longer indexed by satellite.** Selection tracking goes through
  `SatFlarePC::selectedSatIdx` → `sat_flare.comp` writes that satellite's final record into
  `GpuSatListHeader::selected`; the whole 80-byte header is copied into `pickedVisibleBuf` every
  frame (it also carries the real `visibleCount` — the snapshot's `visible_count` was a placeholder
  equal to the roster size until this). Picking copies back only `count` entries + their indices.
- **The sun's flare-source point is its own `vkCmdDraw(1 vertex, firstInstance 1)`**;
  `flare_source.vert` tells it apart by `gl_InstanceIndex`, since the satellite count is GPU-side.
- The header is reset each frame with `vkCmdUpdateBuffer` before `sat_orbit`; the post-orbit
  barrier includes `INDIRECT_COMMAND_READ` at `DRAW_INDIRECT`, which also covers the later draws.

### Two-dispatch pattern (recordCompute)
```
header → satTypeBuf       → CPU memcpy of GpuSatTypeHeader (brightnessScale, mirrorBoost)
vkCmdUpdateBuffer(satListBuf) → reset list header {0, dispatch 0,1,1, draw 0,1,0,0}; barrier →compute
sat_orbit.comp dispatch   → reads satOrbitBuf + satTypeBuf + reflectorTargetsECEFBuf; APPENDS
                            visible satellites to satVisibleBuf/satVisibleIdxBuf, args to satListBuf
                            (+ beams, beamGlowDome)
barrier (3 buffers)       → SHADER_WRITE → SHADER_READ|WRITE (+INDIRECT_COMMAND_READ on satListBuf),
                            compute → compute|DRAW_INDIRECT
vkCmdFillBuffer(glowBuf)  → zeros the glow histogram for this frame
barrier glowBuf           → TRANSFER_WRITE → SHADER_READ|SHADER_WRITE, transfer→compute
sat_flare.comp INDIRECT   → finishes the compact list IN PLACE; glowBuf (atomicMax) + ocean glints;
                            selected satellite's record → satListBuf.selected
barrier satVisibleBuf/satListBuf → SHADER_WRITE → SHADER_READ (vertex) / TRANSFER_READ
point / flare-source / trail draws → vkCmdDrawIndirect(satListBuf.draw*)
end of recordCompute      → copy satListBuf header → pickedVisibleBuf (host, read next frame)
```

### Buffers
| Buffer | Memory | Lifetime | Updated by |
|--------|--------|----------|------------|
| `satOrbitBuf` | device-local | uploaded once; rebaked every 7 sim-days | `uploadSatOrbits()` |
| `satTypeBuf` | host-coherent, mapped | header per-frame; types on upload/rebake | `recordCompute()` / `uploadSatOrbits()` |
| `satVisibleBuf` | device-local | per-frame | compact list: `sat_orbit.comp` appends, `sat_flare.comp` finishes in place |
| `satVisibleIdxBuf` | device-local | per-frame | `sat_orbit.comp` (slot → satellite index) |
| `satListBuf` | device-local, INDIRECT | per-frame | reset by `vkCmdUpdateBuffer`; `sat_orbit.comp` (count/args), `sat_flare.comp` (`selected`) |
| `pickedVisibleBuf` | host-coherent, mapped | per-frame | copy of `satListBuf`'s 80-byte header |
| `reflectorTargetsECEFBuf` | host-visible, mapped | uploaded once at target-generation time | `loadReflectorTargets()`/fallback |
| `glowBuf` | host-coherent, mapped | per-frame | `sat_flare.comp` write; App reads back |

`satOrbitBuf`/`satTypeBuf`/`satVisibleBuf`/`satVisibleIdxBuf`/`satListBuf` are created by
`createSatBuffers()`, sized to the loaded roster — which is why `init()` builds the constellation
**before** `createDescriptors()`. `uploadSatOrbits()` bakes straight into a reused 64 MB staging
chunk (it used to hold two full-size copies — 1.3 GB at 10M) and `buildOrbits()` reserves the exact
total, so a 10M roster's startup no longer spikes host memory.

`mirrorNormalsBuf` (persistent per-satellite mirror lock/slew state) and the old per-frame
CPU-compacted `reflectorTargetsBuf` were both removed 2026-08-06 — see "Subsystem: TargetedReflector
/ Mirror Ground Targets" below. `sat_orbit.comp` now reads target data from a static
`reflectorTargetsECEFBuf` (uploaded once at target-generation time), and carries no persisted GPU
state of its own: every frame's TargetedReflector selection and orientation is a pure function of
that frame's push constants.

### Orbit rebake
`kOrbitRebakeDays = 7`. Each `GpuSatOrbit` bakes `u0 = fmod(orig_u0 + meanMot × epochT0, 2π)` so the shader only adds `meanMot × deltaT` where deltaT < 7×86400 s. `uploadSatOrbits()` auto-triggers in `recordCompute()` when `|simDayJ2000 - orbitEpochDay| >= 7`.

**That product is ~700 rad by day 7, and in plain float it was off by a median 60 m and up to
~770 m along-track** — invisible as a point from the ground, fatal for a mesh next to its own
sprite (Phase 4). `orbitPhase()` (`sat_orbit.comp`) now computes it as an exact two-float product:
`deltaT` arrives as `pc.deltaT` + `GpuSatTypeHeader::deltaTLo` (the low half of the CPU's double),
Dekker's two-product over a Veltkamp split gives `meanMot·dtHi` exactly, and 2π is subtracted as a
14-significant-bit `TWO_PI_A` + remainder so every step is exact. **It must stay `precise`** (SPIR-V
`NoContraction`, 26 decorations): a compiler that contracts or reassociates it silently loses
everything — and it deliberately does not use `fma()`, which Vulkan does not guarantee is fused.
Verified by float32 emulation over 200k satellites × random deltaT in 0-7 days: median 1.1 m, p99
6 m, max 9 m (the float32 floor of the final angle). The header grew 16 → 32 B for `deltaTLo`;
`kSatTypeArrayOffset` follows it automatically.

### simTime representation
Split into `simDayJ2000` (int64_t days) + `simSecInDay` (double, re-based to [0, 86400) each frame). Avoids accumulated float precision loss when a large J2000 base is added to a small per-frame delta. The shader receives `deltaT = float((dDays × 86400) + dSec)` where dDays < 7 (ensured by rebake).

### GpuSatOrbit layout (64 bytes, std430)
All plain floats/uints — no vec3 — so C++ struct packing matches GLSL std430 with no padding.
Must match `SatOrbit` in `sat_orbit.comp` exactly. Only genuinely per-satellite data belongs here —
anything that is constant per type goes in `GpuSatType`.
```
[ 0] raan, u0, R_sat, meanMot
[16] cosI, sinI, cosRaan, sinRaan
[32] tumbleRate, tumblePhase, tumbleAxisX, tumbleAxisY
[48] tumbleAxisZ, alignTerminator, typeIdx (uint), constIdx (uint)
```
`static_assert(sizeof(GpuSatOrbit) == 64)` — do not change field order without updating both structs.

### GpuSatType layout (416 bytes, std430) — `satTypeBuf`
One per `satTypes[]` entry, indexed by `GpuSatOrbit::typeIdx`, at `kSatTypeArrayOffset`: after a
32-byte `GpuSatTypeHeader` (`brightnessScale`, `mirrorBoost`, `occlusionFluxFloor`, `occlusionOn`,
`deltaTLo` —
rewritten every frame; `SatOrbitPC` is full) and the 448 KB `GpuEarthshineLut` (`earthLut` +
`earthShA/B/C` in the shader, written once by `createSatBuffers()`). Must match
`SatType`/`AttGroup`/`SatTypeBuf` in `sat_orbit.comp` (`offsetof` static_asserts guard the C++ side).
```
[ 0] baseColorR, baseColorG, baseColorB, crossSection
[16] specExp0, specExp1, w1, diffuse
[32] mirrorFrac, groupCount (uint), firstLobe (uint), lobeCount (uint — 0 = legacy path)
[48] surfNormalT0 (vec3, triad coords), surfGroup0 (uint)
[64] surfNormalT1 (vec3, triad coords), surfGroup1 (uint)
[80] groups[4] — GpuAttGroup, 64 B each:
     law, primaryTarget, secondaryTarget, jointMode (uint×4)
     jointAxisT (vec3), jointTarget (uint)
     jointVectorT (vec3), jointLimitRad
     jointAngleRad, parent (uint, 0xFFFFFFFF = root), pad×2
[336] originT[4] (vec4 — each group's rest hinge point, triad coords; Phase 3b)
[400] firstOccluder, occluderCount (uint), pad×2
```
Geometry-model lobes live in `satLobeBuf` (binding 8 of the `sat_orbit` set, host-coherent,
`GpuSatLobe` 64 B: normalT + group, area, diffArea, albedoD, f0, alpha2Mat, sampleFirst,
sampleCount, occluderMask, distribution, transmission, occluderMaskHi, pad), packed per type at `firstLobe`; their occluders and sample points in
`satOccluderBuf`/`satLobeSampleBuf` (bindings 9/10), `sampleFirst` rebased at upload.

### GpuSatVisible layout (32 bytes, std430)
A **compact list** entry: appended by `sat_orbit.comp`, finished in place by `sat_flare.comp`, read
by `sat_point.vert`, `flare_source.vert` and the trail splat pass via indirect draws.
```
[ 0] skyDir (vec3) + flareIntensity (float)  — ENU unit vector + intensity [0,1+]
[16] color (uint, packUnorm4x8), angularSize (float, sprite px), rangeM (float), pad
```
Between the two dispatches it is a **pre-photometry** record (culled satellites are not in the
list at all): each carries the raw flux, with `color` already eclipse-tinted and `angularSize` = -1
for a highlighted constellation (else 0). Highlight mode lights satellites normally and raises them to
at least `highlightFlare`; until 2026-09-24 it skipped the lighting and drew every one at that
~mag 5.4 dot, so highlighting a sunlit ISS made it FAINTER. After `sat_flare.comp`, a satellite below
`visThresh` stays in the list as a zero record. Stars and planets use the same record
(`rangeM` = 0, at infinity; `packVisibleColor()` on the CPU). **Phase 4 (2026-09-23):** the tint
was a `vec3`; packing it freed the slot for `rangeM`, which the point draws write as their depth
(see "Unified scene depth").

### GpuSatListHeader layout (80 bytes) — `satListBuf`
```
[ 0] count                                        — append counter = list length
[ 4] dispatchX, dispatchY, dispatchZ              — VkDispatchIndirectCommand (sat_flare)
[16] vertexCount, instanceCount, firstVertex, firstInstance — VkDrawIndirectCommand (3 point draws)
[32] selected (GpuSatVisible)                     — selected satellite's final record, or zeros
[64] selectedRawFlux, selectedRangeM, selectedFound, pad — the selection's PRE-photometry values
                                                    (sat_flare.comp copies them before rewriting)
```
The `selected*` raw fields feed the **GPU-parity readout** (benchmarking M2): where `SatOrbitPC` is
built, `parityPending` records that dispatch's exact inputs (sim time, Sun, observer, tilt,
brightnessScale, mirrorBoost); next frame `updateSelectedPhotometry()` re-evaluates the selection with
`evalSatPhotometry()` at those inputs, shows the physical magnitude in the selection panel, and
computes the GPU−CPU gap (Δmag, a mismatch above `kParityWarnMag` = 0.02 is logged with a 120-frame
cooldown). The gap's readout line was dropped from the panel (2026-09-24): the check and its log
remain, the UI no longer shows it.
Legacy types are compared through the evaluator's `legacyFlux()` mirror (`LegacyReflectance`), in
their own display units. The gap is float-vs-double arithmetic. Until Phase 4 it was chiefly
`u0 + meanMot·deltaT` in float (median 60 m, up to ~770 m along-track by day 7 of a rebake); since
2026-09-23 `orbitPhase()` forms that product exactly (see "Orbit rebake"), leaving ~1 m median /
~9 m max from float32 rounding of the final angle and position.
Mirrored as `SatListBuf` in both `sat_orbit.comp` and `sat_flare.comp`; `offsetof` static_asserts
guard the C++ side.
`static_assert(sizeof(GpuSatVisible) == 32)`

### Push constants

**SatOrbitPC** (128 bytes) — sat_orbit.comp:
```
enuX (vec4), enuY (vec4), enuZ (vec4)  — ECI→ENU basis, offsets 0/16/32
sunDirECI (vec3), deltaT (float)       — offset 48/60
obsECI (vec3), satCount (uint)         — offset 64/76
highlightMask (uint), enabledMask (uint), simDt (float), elevCutoff (float) — offset 80/84/88/92
beamGain (float), reflectorLockWindowS (float), targetCount (uint),
minBeamElevSin (float)                 — offsets 96/100/104/108
gmstNow (float), windowFrac (float), mirrorMaxRateDegPerSec (float), pad2 — offset 112/116/120,
padded to 128
```
2026-08-06 reversibility rework repurposed the fields at offset 100-124 in place (same total size,
no growth): `mirrorSlewDegPerSec` → `reflectorLockWindowS` (a duration instead of a rate — see
below), `activeTargetCount` → `targetCount` (now the full loaded count, not a per-frame
night-side-compacted subset), `mirrorSnap` → `gmstNow` (current-frame GMST, for rotating a target's
static ECEF entry to its live ECI position), `minBeamElevSinRelease` → `windowFrac` (fractional
position within the current lock window, `fract(simTimeAbs / reflectorLockWindowS)`), and (same-day
follow-up) `pad1` → `mirrorMaxRateDegPerSec` — a real angular-rate cap, added once the original
fixed-fraction-of-window crossfade turned out to read as satellites snapping to target (it only
covered one of several transition cases and wasn't derived from actual angular distance — see
"Orientation" below). All of `deltaT`, `gmstNow`, and `windowFrac` are pure functions of absolute
sim time, computed on the CPU in double precision and narrowed to float only after the relevant
periodic reduction — so `sat_orbit.comp` can extrapolate exactly to a lock window's start instant
(see below) with no persisted GPU state anywhere in the pipeline.

**SatFlarePC** (128 bytes) — sat_flare.comp:
```
enuX (vec4), enuY (vec4), enuZ (vec4)  — offsets 0/16/32
sunDirECI (vec3), satCount (uint)      — offset 48/60
obsECI (vec3), selectedSatIdx (uint)  — offset 64/76
pad3, daySuppression, pad4, visThresh, highlightFlare,
extinctionCoeff, moonSuppression, pad0 — offsets 80–108
moonDirECI (vec3), sunRefIntensity     — offset 112/124
```
`pad3`/`pad4` were `brightnessScale`/`mirrorBoost` until the lighting overhaul's Phase 1 moved the
reflectance model into `sat_orbit.comp`; offset 76 was `elevCutoff`, then a pad, and since Phase 1b
is `selectedSatIdx` (`UINT32_MAX` = none).
`pad2` was `lightPollution` — superseded (session 26) by the directional `lightDomeBuf` SSBO
(binding 3 in the sat_flare.comp descriptor set, 8 floats, host-visible/mapped), which doesn't
need push-constant space. See "Subsystem: Light Pollution Dome" below.

**SatDrawPC** (128 bytes) — `sat_sky.vert`/`.frag` (+ `_lite`/`_minimal`) via `skyBgPipeLayout`:
```
skyView (mat4)                          — offset 0
fovYRad, aspect, gmst, waveTime         — offsets 64/68/72/76
sunDirENU (vec4) — xyz=dir, w=sin(el)  — offset 80
moonDirENU (vec4) — xyz=dir, w=illum   — offset 96
obsECEFDir (vec4) — xyz=ECEF, w=obsHeightOffset — offset 112
```
Exactly the 128-byte `maxPushConstantsSize` floor (oldest AMD integrated parts). Everything that
used to trail past offset 128 — `debugDisableMask`, `screenSizePx`, `skyGlareVisibility`, the four
`beam*` scalars, `mwSuppressEased` — was per-frame-uniform and moved into the **CloudParams UBO**
(`cloud_params.glsl` / `GpuCloudParams` "Push-constant relief" block): `sat_sky.frag` reads them as
`cloud.dbgDisableMask` / `vec2(cloud.skyScreenW, cloud.skyScreenH)` / `cloud.skyGlareVisibility` /
`cloud.beam*` / `cloud.mwSuppressEased`. `buildSkyDrawPC()` fills it.

**PointDrawPC** (128 bytes) — `sat_point.vert`/`.frag` (`drawPipeLayout`) + `star_point.vert`/`.frag`
(`starPipeLayout`, also planets/trail):
```
skyView (mat4)                          — offset 0
fovYRad, aspect, waveTime, noTwinkle    — offsets 64/68/72/76
moonDirENU (vec4)                       — offset 80
obsECEFDir (vec4) — w=obsHeightOffset   — offset 96
screenSizePx (vec2)                     — offset 112   (always ctx.swapExtent — point draws never scale)
debugDisableMask (uint)                 — offset 120   (only sat_point.frag's knockout bit 4096)
manualTerrainTest (float)               — offset 124   (1 on the trail draws only)
```
Split from `SatDrawPC` so both point pipeline layouts fit 128 bytes while still carrying the two
per-draw flags that genuinely can't be frame-UBO'd: `noTwinkle`=1 on the planet draw, and
`manualTerrainTest`=1 on the trail draws — both differ between draws within one frame.
`buildPointDrawPC()` fills it; callers set the two flags. Each point shader declares only the
prefix it reads.

---

## Subsystem: TargetedReflector / Mirror Ground Targets

Mirrors in `TargetedReflector` mode aim at a ground target chosen by each satellite. All
per-satellite selection and orientation computation runs in `sat_orbit.comp`.

**2026-08-06 reversibility rework.** The previous design (lock + acquire/release hysteresis in
`mirrorNormalsBuf`, rate-limited slew integrated frame-by-frame) was persistent, history-dependent
GPU state: which target a satellite held and how far its mirror had slewed toward it were both a
function of the *sequence of frames* used to reach the current sim time, not of that sim time
alone. Playing forward to an instant and reaching the same instant by reversing time therefore
accumulated different lock/slew histories and could show different satellite/target pairings —
structural, not a tunable-away bug, since hysteresis is deliberately not time-symmetric. Separately,
the per-(satellite,target) preference hash (`hash11(float) `on a large combined index) silently
collapsed to a constant `0.0` for every candidate once a satellite's global dispatch index exceeded
~15,000 float32 mantissa range — Reflect Orbital satellites sit at index ~57,000+ given the
constellation upload order, so EVERY score was tied and argmax degenerated to "first eligible
candidate in scan order," meaning only the lowest-original-index site among a satellite's
simultaneously-eligible set ever won and other co-eligible sites were never picked.

Both are fixed together: `mirrorNormalsBuf` and the old per-frame CPU-compacted
`reflectorTargetsBuf` are gone. TargetedReflector selection and orientation are now pure functions
of the current frame's push constants (themselves pure functions of absolute sim time), so forward
and reverse playback of the same instant produce bit-identical results, and the hash is an
all-integer mix (`pairScore`, `sat_orbit.comp`) immune to magnitude collapse.

### Target generation (once at init) — S1, RELEASE_v1_1_PLAN.md
`SatelliteSim::loadReflectorTargets()` reads `reflector_targets.json` (next to the exe, moddable
exactly like `constellations.json`) — a hand-curated list of ~50 real, publicly-known solar
installations (`{name, lat, lon, capacity_mw}`), with the first entry flagged
`"observer_spawn": true` at the exact fixed spawn point (67°S, 67°W — see "Fixed Simulation
State"). Falls back to `generateReflectorTargetsRandomFallback()` (uniformly-random ECEF points,
still with a real fixed entry at index 0 for the observer-spawn pin) if the file is missing,
malformed, or empty. `kNumReflectorTargets = 201` is a **capacity** (buffer sizing), not the real
count — `reflectorTargetCount` (≤ capacity) holds how many actually loaded.
`reflectorObserverSpawnIdx` records which loaded index is the pin (informational/logging).

Per-target ground radius (`reflectorTargetsRadiusM[]`, real terrain elevation via a 3×3-max
`earthElevCpu` lookup) is computed by the shared `computeReflectorTargetElevationRadius(ti)`
helper — used by both the JSON path and the fallback. Both xyz (unit ECEF direction) and this
radius are uploaded ONCE, at generation time, into `reflectorTargetsECEFBuf` (host-visible,
`vec4` per target: xyz=ECEF dir, w=radius) — read by `sat_orbit.comp` for TargetedReflector target
search. There is no per-frame CPU rotation step any more; `sat_orbit.comp` rotates ECEF→ECI itself,
on demand, for whichever instant it needs (see below).

### Target kinds and solar PV parks (2026-10-02)

Every target carries a `"kind"`: `solar` (58 PV parks, Casa Grande included), `agriculture` (the two
agrivoltaics points), `daylight` (the seven "polar illumination" towns) or `none` (the spawn pin);
`reflectorKind[]`. A file without it is inferred from the name and `capacity_mw`, so old and modded
lists still load. Only `solar` changes anything so far: the park is drawn on the terrain.
- **Data:** area = `capacity_mw` x 2.2 ha/MW (or `"area_km2"`), mount = single-axis N-S trackers in the
  Americas, India and Australia, fixed tilt facing the equator elsewhere (or `"mount"`). Ivanpah, Noor
  Ouarzazate and Cerro Dominador carry `"tech": "csp"` and are still drawn as PV.
- **CPU (`fillSolarSites`, end of the beam readback):** the 8 nearest solar sites within 2500 km, in
  double, as `GpuSolarSite` (80 B) appended to `GpuGroundBeams` (`solarCount` = the old pad0) — no new
  binding (`sat_sky.frag` is at its sampled-image floor and has many SSBOs). The centre is relative to
  the observer's SEA-LEVEL point, the frame of `terrainQ`, so the shader's difference is float-exact and
  the rows are world-fixed. Trackers turn toward the Sun while it is up at the site, else toward the
  strongest beam landing on the park, else stow flat, eased over ~8 s, clamped +-55 deg.
- **Shader (`solarSiteAt`, before the farm block, which gives way inside a park's plots):** plots of
  4 x 4 blocks in or out of a wobbly outline, blocks of whole rows with 6-m roads, rows and segments as
  exactly box-filtered pulse trains (`solarPulseCover`: no aliasing at any footprint, a dark patch from
  orbit). Coverage is AS SEEN: a panel of width W tilted to n covers W |nz + nx dx/dz| of ground along the
  view (viewed across the rows the panels hide the ground), and casts the same projection along the Sun
  as shadow. Glass (`solarGlint`, Beckmann + Schlick F0 0.03 + Smith, the sea glint's form): sky
  reflection and Sun glint in the terrain lighting; in the ground-spot loop each beam's light
  (`GpuGroundBeam::dirOct`, the old pad0: ground -> satellite, octahedral snorm16) glints off the panels,
  and the spot's diffuse light now follows the surface's brightness (`solarDiffK`; it was the same on
  every surface). Seen from the mirror direction, each beam's glint lands on other rows, so the park
  shows the ring of beam satellites as a glitter band. Per-row tilt jitter +-1.5 deg; unresolved, it
  widens the lobe. Backs are a grey backsheet with no glass reflection. Debug view 27 (`debugview
  solar`): R coverage, G park ground, B normal east. Setting `clouds.solar_arrays` (bool, no UI yet).
  Cost within noise (harness `perf`, Topaz from 800 m). `scripts/solar_parks.satcmd`.
- **Rooftop PV where a park overlaps a city (2026-10-03):** the site evaluation runs BEFORE the city
  day pattern. With the city's presence `solarUrbanK` (cityLum, after the terrain limit), ground blocks
  drop out block by block (a hash per block below urbanK) and the park's smooth radial density
  (`radK`) x urbanK goes to the city pattern as `gCityPvK` (globals beside `gCityViewE`). `cityDayGrid`
  then puts panels on its own roofs, per lot by hash: commercial / perimeter / slab roofs rows of tilted
  panels at a 2.2-m pitch over most of the roof, houses one array on one slope, off-centre. It writes
  `gCityPvCov` (footprint-filtered; expectation `kCityPvMean` x K once unresolved, so it holds to orbit)
  plus a per-roof tilt jitter; after the farms the panels darken the albedo and join the park's glass
  (normal ~15 deg toward the equator, +-8 deg per roof; a broad lobe when unresolved). Reference: the
  user's Odessa snapshot (Roadrunner's coordinates sit on the city; the real plant is in Upton County).
  Cost within noise there.
- **Ground spots land on the target's ground (fixed the same day):** the CPU traced every beam to the
  sea-level sphere, so a beam arriving at 11-20 deg on a site 1.7 km up (Villanueva) lit the ground ~5 km
  past it. The ground spot and the cloud lights now intersect the sphere of the target's own radius
  (`reflectorTargetsRadiusM` minus its 75-m margin). The sky shafts still end at R_EARTH.

### Per-satellite selection (GPU, sat_orbit.comp) — deterministic lock windows
Target IDENTITY is chosen per fixed-width **sim-time window**
(`SatOrbitPC::reflectorLockWindowS`, default 90s, settings-window "Target lock window (s)"), not by
a persisted per-satellite lock.

**Per-satellite phase offset (2026-08-06 same-day follow-up).** `SatOrbitPC::windowFrac` is one
GLOBAL value, identical for every satellite dispatched this frame — without correction, every
TargetedReflector satellite's window boundary lands at the exact same sim-time instant, so all of
them ease toward a new target simultaneously: reported as one large synchronized wave of motion
regardless of how `reflectorLockWindowS`/`mirrorMaxRateDegPerSec` were tuned (a short window reads
as constant chaos, a long one as periodic mass movement). Fixed with a per-satellite hash offset —
`windowFracI = fract(pc.windowFrac + hashU(i × 0x2545F491u)/2³²)` — which is exactly the fractional
part of `(simTimeAbs + offsetSeconds_i) / W` (dropping the integer part doesn't care which multiple
of `W` `offsetSeconds_i` came from), i.e. this satellite's OWN window fraction, needing no extra
CPU-side work or push-constant fields. Every use of `windowFrac` in this section below is really
`windowFracI` in the shader; still a pure function of (sim time, satellite index), so still fully
reversible.

`sat_orbit.comp` extrapolates this frame's `deltaT`/`gmstNow` back to the CURRENT window's own
START instant — `toWinStart = -windowFracI × reflectorLockWindowS`, then
`evalDeltaT = deltaT + toWinStart` and `gmstEval = gmstNow + K_OMEGA_EARTH × toWinStart` (and one
more `reflectorLockWindowS` further back for the PREVIOUS window's start). **This extrapolation is
exact, not approximate**, for both quantities in this sim's model: orbital phase
(`u = u0 + meanMot×deltaT`) and GMST are both exactly linear in time, so evaluating at a shifted
`deltaT` is algebraically identical to the CPU having computed `deltaT` relative to a different
reference instant — no precision or physical approximation beyond what `deltaT`/`gmstNow` already
carry. The one real approximation is treating `sunDirECI` as constant across one window (true to
within the sun's ~1°/day drift against a ~90s window).

At each eval instant, `findWinner()` re-derives the satellite's own ECI position (`satEciAt`, same
closed-form math as the real position, evaluated at `evalDeltaT`) and scans **every** loaded target
(`pc.targetCount`, not a pre-filtered subset — the old per-frame CPU night-side compaction is gone),
rotating each target's static ECEF entry by `gmstEval`, rejecting day-side-at-that-instant and
anything below `pc.minBeamElevSin` (local elevation of the satellite *as seen from the target*), and
taking the `pairScore(satIdx, ORIGINAL target index)` argmax among survivors. Called once for the
CURRENT window's start (`bestIdx`) and once for the PREVIOUS window's (`bestIdxPrev`) — both
constant across their own whole window, since they only depend on that window's own start instant,
not on which frame within it asks. `pairScore` doesn't depend on the eval instant at all, only on
which targets are eligible — so a satellite's top-scoring target keeps winning unprompted across
consecutive windows as long as it stays eligible; a window boundary only actually changes the
winner when the incumbent drops out (day-side, or below `minBeamElevSin`).

`pairScore` is a pure function of the `(satellite, target)` pair (all-integer hash, see the block
comment above `hashU`/`pairScore` in `sat_orbit.comp`), independent of scan order and of how many
other candidates exist — the same load-spreading property the old `hash11`-based score was designed
to have, minus the magnitude-collapse bug.

**Beams need sunlight (2026-10-04):** a beam's intensity is x the satellite's Earth-shadow `litFactor` (the
soft umbra/penumbra, now computed before the ground-site block): until then a mirror in the Earth's shadow beamed
at full strength (user snap: Reflect satellites lighting the Antarctic winter night).

### Orientation — a rate-limited ease, not integrated slew
2026-08-06 same-day follow-up: the first cut of this rework smoothed only ONE transition case (both
the current and a look-AHEAD "next window" valid and different) via a fixed-fraction-of-window
crossfade, unrelated to how far the mirror actually had to swing. Every other transition (acquiring
a target from nothing, losing one, falling back to the nearest night-valid site) snapped instantly,
and even the smoothed case could look abrupt for a wide swing compressed into a fixed ~13.5s —
reported as satellites visibly snapping to target. Replaced with a genuine angular-rate cap
(`SatOrbitPC::mirrorMaxRateDegPerSec`, settings-window "Mirror max slew rate (deg/s)") applied via a
closed-form ease, covering every transition uniformly:
- `bestIdxPrev` (previous window's winner — a one-window lookback, not unbounded history, so still
  a pure function of current sim time) gives `startAim`: the ideal aim direction toward whatever the
  mirror was presumably doing right as the CURRENT window began, evaluated AT the current window's
  own start instant (`idealTowards(satEciCur, targetPosAt(bestIdxPrev, gmstEvalCur), ...)`).
  `nearFallbackIdeal()` (nearest night-valid target, no elevation gate, or `FlatMirror45` if truly
  nothing night-valid exists) substitutes whenever the relevant index is `< 0`, used identically for
  "previous window had nothing" and "current window has nothing."
- `destAtStart` is the same computation for `bestIdx` (current window's own winner) at that same
  start instant — directly comparable to `startAim` since both are evaluated at the identical
  moment, only the target differs. `angle0 = acos(dot(startAim, destAtStart))` is therefore a
  single stable angle for the whole window, not something that recomputes every frame.
- `slewDuration = clamp(angle0 / radians(mirrorMaxRateDegPerSec), ~0, reflectorLockWindowS)` — the
  real time a slew at the configured max rate would take, capped at one window so a huge swing
  can't bleed into the NEXT window's own (independently computed) ease.
- The mirror eases from `startAim` toward `liveIdeal` (the target's true LIVE, unquantized position
  right now — this is what makes it track exactly once caught up, not toward a stale window-start
  snapshot) via `smoothstep(0, slewDuration, windowFrac × reflectorLockWindowS)`. When
  `bestIdxPrev == bestIdx` (the common case — nothing actually changed), `angle0` is exactly 0, the
  ease completes within a clamped instant, and the mirror simply tracks `liveIdeal` for the whole
  window, matching the previous design's "once slew has caught up" steady state.
- `bestIdx` (for beam bookkeeping — footprint, `beamCloudBlock` lookup) is always the CURRENT
  window's winner, using its live position, regardless of ease progress — only ORIENTATION lags
  during a transition, never where the beam is drawn. `aimErrorRad` is the residual angle between
  the eased orientation and `liveIdeal` (0 once caught up) — same field, same downstream consumer
  (`cloud_march.comp`'s beam debug ray fade) as before, just driven by the rate-limited ease instead
  of window-crossfade progress.

Because every one of these quantities — `windowFrac`, `evalDeltaT`/`gmstEval` (current AND
previous), `bestIdx`/`bestIdxPrev`, `angle0`, the ease itself — is a pure function of the current
frame's push constants (themselves pure functions of absolute sim time computed in double precision
on the CPU), the whole pipeline remains reversible by construction: there is nothing to accumulate
differently depending on playback direction, even though the visual result now has a genuine
physically-motivated slew rate.

---

## Subsystem: Reflect-Orbital Beam Cloud Occlusion

See `.plans/BEAM_CLOUD_PLAN.md` for the full session-by-session history — this section is the
current-architecture summary only.

**The visible beam is not called "the debug ray" despite its name.** `showBeamDebugRays`
(`SatelliteSim.h`) started as a literal debug-only visualization (a green line, C12 follow-up #12)
back when a separate volumetric "tube" was the real beam visual. Once that tube was thrown out for
graphics/performance reasons, this ray was reworked in a later session (realistic color, altitude
attenuation) into the actual production beam visual — it defaults to `true` and is what a player
sees, not a diagnostic overlay. The shader-side header comment in `cloud_march.comp` calling it
"Opt-in and off by default" is itself the stale artifact of that history, not a bug — a same-session
pass mistakenly "fixed" the default to `false` reasoning from that comment before this was
clarified (2026-08-09); do not repeat that mistake. This ray was, until 2026-08-09, occluded by
terrain ONLY — genuinely no cloud awareness at all — which is almost certainly what a string of
"beams pass through clouds no matter what" reports were actually seeing, independent of whatever
was fixed in the occlusion math underneath it.

**`beam_self_march.comp`** (2026-08-09) computes real per-BEAM cloud occlusion, replacing
`beam_cloud_block.comp`'s per-TARGET vertical-column approximation (deleted). Dispatched over
`BEAM_MAX_ACTIVE` (2048) threads right after `sat_orbit.comp` (needs the `satENU`/`reflectDirENU`
that shader just wrote), fixed-size every frame (`beamCount` is a GPU atomic counter, not known at
command-buffer record time — inactive slots return immediately). For each active beam it
reconstructs the satellite's true ECEF position from `ReflectBeamsBuf`'s observer-relative ENU
offset (via `terrain.glsl`'s `observerPos()`/`enuBasis()`), then marches the SEGMENT from the
satellite to the mirror's **actual current ground intersection** — `reflectDirENU` traced to
`R_EARTH` via `raySphere`, NOT the chosen target's fixed `targetENU` position — through the cloud
shell, overwriting `blockAltM`/`blockOpacity` on that same `ReflectBeam` entry in place.

**Marching toward `targetENU` instead of the real ray was a second, subtler bug**, found in-app
the same day after the ground-intersection fix above already shipped: occlusion looked correct for
a beam locked onto its target (where `reflectDirENU` and the target direction coincide) but did
nothing for a beam still slewing between two targets (where they genuinely diverge — see
`aimErrorRad`/the TargetedReflector orientation section above). The march was checking cloud along
a path the beam wasn't physically following. Fixed by using `raySphere(satECEF, reflectDirECEF,
R_EARTH)`'s hit point as the near endpoint instead of `targetECEF` — same shell-crossing math
otherwise (the hit point is still guaranteed inside both cloud-base/cloud-top spheres, same as
before), just anchored to reality instead of intent.

**Why per-beam, when "Deliberately NOT per-satellite" was the standing design rule** (see
`beam_cloud_block.comp`'s own retired header, and `TERRAIN_PLAN.md` follow-ups #14-#16): that rule
was about a DIFFERENT failure shape. Follow-up #14's cost blowup was a real `cloudDensity()` march
evaluated per SCREEN PIXEL near each beam's line, once per satellite — cost multiplied by
(satellites × pixels). Follow-up #16's flicker came from a dedup fix that picked one "winning"
satellite's geometry per target per frame, and the winner's identity flipped frame-to-frame among
near-equal candidates. `beam_self_march.comp` is neither: a single bounded march per beam (same
shape `beam_cloud_block.comp` itself already proved cheap at 201 threads — beam_self_march is
~10x the thread count along a segment instead of straight up, still well under 1ms against
`cloud_march.comp`'s own ~16ms), with zero arbitration — every beam computes its own value from its
own stable geometry, so there is nothing to flicker between.

**`blockAltM`/`blockOpacity` keep their pre-existing meaning** (altitude where the path's
transmittance first drops below 50%, and overall 0-1 opacity — including the weighted-mean-
absorption-altitude fallback for a column that never crosses that threshold, so a thin/scattered
cloud doesn't produce a fake cutoff edge pinned to the shell's nominal ceiling). `sat_sky.frag`'s
ground-spot `shadowAtten = 1.0 - groundBeams[bi].blockOpacity` needed zero changes and automatically
reads more physically-accurate per-beam data.

**The visible pointing ray and the volumetric cloud glow were reworked together, same day, per
explicit user direction, once the march above was marching the real path.** Both previously read
NOTHING from `blockAltM`/`blockOpacity` (the ray) or read it through a per-TARGET CPU aggregation
with no directionality at all (the old `beamCloudLighting()`/`BeamCloudLightBuf` glow — a purely
horizontal Gaussian around the target's ground position, gated by a height cutoff averaged/argmaxed
across every satellite servicing that site). The visible ray and the cloud glow ended up on two
DIFFERENT architectures, after an intermediate design that didn't work out:
- **The ray** stayed in `cloud_march.comp`'s `main()`, per-pixel, folded into its existing
  closest-approach loop (`showBeamDebugRays`-gated). It applies a height-cutoff `smoothstep`/`mix`
  shape at its own closest-approach altitude (`qAltM`, already computed there for the vacuum/
  altitude fade), ADDITIONALLY multiplied with the pre-existing `cloudGate` term — the two answer
  different questions and neither substitutes for the other: `cloudGate` gates whether the
  CAMERA's own line of sight to this point is blocked by unrelated cloud; the height-cutoff term
  gates whether the BEAM's own sunlight survives to reach this point along its real path. Both
  must be open for the ray to render there.
- **The cloud glow went through a second design that shipped and was reverted the same day.**
  First attempt folded it into that same per-pixel ray loop, reusing `perpDist` (the CAMERA's
  view-ray-to-beam-line distance) for proximity — cheap, but geometrically wrong: that distance's
  iso-contours are not soft blobs, they're hyperbola-like curves, and it rendered as large white
  rings intersecting the beams. Explicit user correction: this needs to work exactly like the
  sun/moon terms already do, evaluated PER CLOUD SAMPLE inside the volumetric march, not as a
  screen-space effect. Reverted outright (not patched) back to the ray-only per-pixel loop.
- **The glow's current (third) design restores the per-sample shape** (`beamCloudLighting()`,
  called from `cloudMarchCS`'s own loop and added into `inScatter` right alongside
  `sunColorCloud`/`moonContrib` — the "feed forward" the user asked for), fed from a small
  CPU-built list (`SatelliteSim.cpp`, `kMaxCloudBeamLights`=**512**, `GpuBeamCloudLights`; this
  doc said 16 for a long time and was simply wrong — the cap was raised for coverage and the
  number here was never updated, which is exactly why that function's real cost went unnoticed) — the only
  shape proven affordable at per-march-sample call frequency (TERRAIN_PLAN.md follow-ups #14/#16).
  Each list entry is now ONE individual real beam (no per-target aggregation, so nothing to
  average/argmax and nothing to flicker between): `posENU` is that beam's REAL ground intersection
  (`satENU + reflectDirENU` traced to `R_EARTH`, via the same rotation-invariant local-frame
  `raySphere` trick the GPU shaders use — valid on the CPU too, no ECEF conversion needed), and
  `dirToSource` is its REAL direction (ground toward satellite), driving `phaseCloud()` per light
  instead of a shared `normalize(p)` local-zenith stand-in the old per-target version used.
  (**"ONE individual real beam per entry" is history, not current behaviour** — clustering was added
  2026-08-09 and reworked 2026-08-12. The per-beam GEOMETRY described here is still exactly what
  feeds the list; what an entry represents is not. See "Cloud-light identity and cross-frame easing"
  below.)
- `sat_sky.frag`'s ground spot separately anchors at the same real ray-ground intersection
  (`raySphere(satWorldPos, reflectDirENU, R_EARTH)`) instead of `targetENU` — this part of the fix
  was correct from the start (not implicated in the ring bug, a genuinely different computation)
  and was left alone through the glow's redesign. The range-cutoff fade (`beamMaxRangeM`)
  deliberately stays keyed to the fixed target site position, not the transient ray-ground point —
  that's "is the observer close enough to this SITE," which shouldn't flicker as a mid-slew ray
  briefly touches down elsewhere.
- That pre-existing gap — knockout bit 128 gates `sat_sky.frag`'s ground-spot loop and
  `beamCloudLighting()`'s per-sample glow, but NOT `cloud_march.comp`'s per-pixel ray loop, which
  had only ever been gated by `showBeamDebugRays` — **was closed 2026-08-10 by knockout bit 8192**,
  and measuring it is what found the cost below.

### Cloud-light identity and cross-frame easing (2026-08-12)

`GpuBeamCloudLights`, the GPU-side struct and its 512 cap, is **unchanged** — so is every consumer
(`beamCloudLighting()`, `cullCloudLightsForTile`). What changed is how the CPU builds it. Full
session history in `.plans/BEAM_CLOUD_PLAN.md`; this is the architecture summary.

The list used to be rebuilt from scratch every frame, and a cluster's identity was **emergent**: it
was seeded by whichever beam the scan reached first at a target (a 2 m `targetENU` epsilon match)
and admitted members by comparing them against a running partial average that changed as members
were added. That partition is discontinuous in its own inputs — one satellite dropping out
repartitions the survivors, changing cluster count, every direction and every summed intensity in a
single frame. Since `posENU`/`dirToSource`/`blockAltM`/`blockOpacity` are all intensity-weighted
means and `hgG≈0.99`, that reads as lights popping and re-aiming instantly. **A 2026-08-11 attempt
to fix this by adding a fade on top was reverted** (still flickery, 20 FPS): easing a slot whose
*meaning* changes underneath it doesn't help, and recovering identity by proximity-searching a pool
cost O(rawClusters × 256) per frame.

Identity is now **declared**, which is possible because `sat_orbit.comp` carries its `bestIdx`
through as `ReflectBeam::targetIdx` (**this is why the record grew 64 → 80 bytes** — the three pads
next to it are load-bearing, see below):

| | key | why it's stable |
|---|---|---|
| cluster | `(targetIdx, direction bucket)` | bucket is a FIXED quantization of the beam's direction in the **target site's own** local ENU frame (`reflectorSiteEnu*[]`, computed once in `computeReflectorTargetElevationRadius()`) — a pure function of that beam's geometry, independent of scan order, of the cluster's contents, and of the observer |
| individual (transiting) | originating satellite dispatch index (`debugPad`) | already guaranteed stable by `sat_orbit.comp`, unlike the atomic-append slot `s` |

`TrackedBeamLight` (`SatelliteSim.h`) holds the persistent state: two pools with the same reserved
budgets as before (256 + 256 = `kMaxCloudBeamLights`, so the emit can't truncate), each with a
power-of-two open-addressed key→slot index **rebuilt from live slots every frame** — O(live) ≤ 256,
and it avoids tombstones entirely. Values are eased with the `1 - exp(-dt/τ)` idiom
(`mwSuppressEased`'s): intensity asymmetric (`beamClusterFadeInS`/`OutS`, Beams tab), geometry on a
shorter fixed `kTrackedLightGeomEaseS`.

**Three invariants:**
1. **Geometry is stored in Earth-fixed ECEF, in `glm::dvec3` — never observer-relative ENU.** A
   ground site is stationary in ECEF, so an entry that goes unmatched for its whole fade-out cannot
   drift however far or fast the observer moves. `rebase()` still applies to the raw per-beam fields
   (genuinely one frame stale) and must NOT be applied to tracked state: it is rotation-only, built
   for exactly one frame of lag, and the 2026-08-11 revert burned three rounds relearning that.
2. **`ReflectBeam`'s three explicit `uint` pads must exist in BOTH `reflect_beam.glsl` and
   `GpuReflectBeam`.** std430 rounds the GLSL struct up to its 16-byte alignment; C++ does not,
   because `glm::vec3` is 4-aligned. The pre-`targetIdx` total agreed at 64 only by luck. Same
   silent-permutation hazard as `GpuCloudParams`, and the `static_assert` only catches a size change.
3. **Nothing in the readback loop may depend on scan order again.**
4. **The raw per-beam positions are rebased RIGIDLY (review 7):** `satENU`/`targetENU` are positions
   relative to last frame's EYE, so `rebasePos` adds the eye's own move (double, `lastBeamObsRadius`)
   to the basis turn; `rebase` (rotation only) is right for `reflectDirENU` alone. Rotation-only
   positions lagged the observer's per-frame displacement — ~2 km a frame flying at the height-scaled
   speed from 116 km, and the ground spots visibly swung while strafing.
5. **One eye for satellites and shaders:** `obsECI` (and every CPU evaluator: trace, selection
   readout, harness Sun search) uses `obsEyeRadiusM()` = R + max(ground, height offset) + 2 m, the
   shaders' `observerEffHeight`. It was R + ground + offset: over the Sierra a ~3 km different eye, so
   beams landed km off the drawn scene and jumped as the CPU ground under the observer changed. The `std::sort` by `debugPad`
   that used to enforce determinism is deleted — the partition is order-independent now (sums and a
   max), `groundTopK` ranks on intensity, and `nearest`/the opacity diagnostics are commutative.

`beamClusterDirThresholdDeg` kept its slider, label, range and settings key, but its **meaning
changed**: it was the merge tolerance against a running-average direction, it is now the angular
size of a fixed bucket. Settings → Beams also shows live pool occupancy, which is the instrument for
the failure mode that killed the previous attempt — a count pinned at 256 means entries are
respawning instead of matching; a count near the real active-site count means keying is working.

Accepted: the eased list is history-dependent, so it is **not** bit-reversible under time reversal
the way the orbital pipeline is. Same class and precedent as `skyGlareEased`/`mwSuppressEased`.

### Beam pointing-ray tile culling (2026-08-10)

The Anchorage worst-case sweep measured that per-pixel ray loop at **7.54 ms at Medium — 28% of the
whole frame**, and 4.72 ms of Planetarium's ~10.9 ms (43%). It ran `min(beamCount, 2048)` iterations
on every half-res texel (571 beams × 484,800 texels = **277M iterations/frame**) and rejected on
distance only at the END, after ~7 SSBO loads, a `raySphere` with a sqrt and the full
closest-approach solve. Beam rendering in total (this loop + bit 128's two consumers + the
`beam_self_march.comp` dispatch) was **14.0 ms, 52% of the Medium frame** — more than the volumetric
cloud march.

`cullBeamsForTile()` is the standard Forward+ light-cull answer, and it fits for free because this
shader is already dispatched at `local_size 16x16`: one workgroup owns a 16×16-texel screen tile, so
its 256 threads cooperatively test every beam ONCE against the tile's bounding view cone and leave a
short shared-memory list (`sTileBeamIdx`/`RayLen`/`Fade`, cap `kTileBeamMax`=384, ~4.6 KB shared)
that each thread walks. Per-thread iteration count becomes "beams crossing this tile" rather than
"beams that exist", so **cost stops scaling with the observer's location** — Anchorage concentrating
beams was the entire problem.

It also does the **Tier-1 hoist**: `rayLen` (a `raySphere` against `R_EARTH`), `targetDistM`/
`rangeFade` and `aimFade` are per-beam constants the old loop recomputed once per texel — 484,800
times each. They are now computed once per workgroup and passed through shared memory. Doing the
hoist here rather than in `beam_self_march.comp` needs no new `ReflectBeam` fields and no
producer-side plumbing, and — the deciding reason — does **not** make bit 512 (that dispatch's
knockout) an unsafe fallback.

**Three invariants, all easy to break:**
1. **`kDebugRayRadiusM`/`kDebugRayMinAngRad`/`kDebugRayMaxLenM`/`kDebugRayAimMaxRad`/`kSkyBeamFadeM`
   moved to file scope** precisely so the cull and the per-texel loop cannot disagree. If they
   diverge the cull stops being conservative and beams pop at tile boundaries.
2. **The conservatism bound.** Two rays sharing an origin and diverging by at most the tile
   half-angle are separated by at most `tFar * tileHalfAngle` anywhere within `tFar`; the closest
   point on the view ray to any point of the beam segment lies within `tFar` of the origin. So the
   accept test uses `radiusMax*4 + tFar*tileHalfAngle` with `radiusMax` derived from `tFar` (not the
   centre ray's own `t`). The failure mode is unmistakable: beams appearing/disappearing on a
   16×16-texel grid.
3. **The barriers must stay in uniform control flow.** `cullBeamsForTile` is called BEFORE `main()`'s
   out-of-bounds early return, so every thread of an edge workgroup reaches both `barrier()` calls;
   the scan itself sits in a push-constant-only (workgroup-uniform) branch, and the barriers are at
   the function's top level, outside it. `obsEffH`/`obsPos` are therefore also resolved above that
   early return — safe, since `observerEffHeight` depends only on `pc.obsECEFDir`, not on `coord`.

Overflow past `kTileBeamMax` sets `sTileOverflow` and falls back to scanning the whole buffer,
recomputing exactly what the cull would have supplied — a pathological tile gets slow, never wrong.
**Knockout bit 131072 forces that same fallback**, which makes it the cull's correctness A/B (image
must be pixel-identical with it on) and the way to measure what the cull bought — its sweep
`cost_ms` is the SAVING, reported with the opposite sign to every other row.

**Measured:** beam pointing rays 7.54 ms → **1.08 ms** at Medium; the in-sweep A/B says the cull
saves **6.78 ms** there and **3.29 ms** at Planetarium (which dropped 10.9 → 6.85 ms total).

### Cloud-light tile culling (`cullCloudLightsForTile`, 2026-08-10)

Same workgroup, same pattern, different list. `beamCloudLighting()` is called from `cloudMarchCS`'s
innermost loop — once per in-cloud SAMPLE — and walked all `beamLightCount` entries every time,
against a cap of **512**. That was 3.48 ms of the `cloud_march` bucket at Medium. The cull reduces
512 lights to the handful whose influence cylinder can reach any ray in the tile; the per-sample
loop walks that shared list instead.

Its conservatism argument differs from the ray cull's in one place worth understanding:
`tRangeMax` is the tile-centre ray's own far crossing of the **cloud-top sphere**, which
upper-bounds where `cloudMarchCS` can march (`tExit` is min'd against exactly that shell exit, and
both `tScene` and `maxRenderDistM` only shorten it), scaled by `kTileRangeMargin` = 1.25 so a
tile-edge ray whose own shell crossing runs slightly longer is still covered. `kBeamCutoffSigma`
moved to file scope for the same reason the ray constants did — the cull and the per-sample test
must bound against the identical radius. Bit 131072 disables this cull too.

### Beam ground-spot CPU hoist (2026-08-10)

`sat_sky.frag`'s ground-spot loop measured 1.59 ms at Medium, on a `GroundBeamsBuf` sitting at its
full `kMaxGroundBeams` = 256 cap, so every ground-hit pixel paid all 256 iterations at full
resolution. Almost the entire body was view-INDEPENDENT: a `length(targetENU)` + smoothstep range
fade, an `obsPos + satENU` and a `raySphere` (two sqrts) for the real ray/ground intersection, the
elevation fade, and the shadow attenuation. Only the horizontal distance to the landing spot and
the two Gaussians built from it genuinely vary per pixel.

All of it moved to the CPU loop that already builds this buffer each frame, into a new packed
`GpuGroundBeam` (32 bytes, mirrored as `GroundBeam` in `sat_sky.frag` — hand-mirrored, same
convention and same hazard as `GpuCloudParams`): `weight` (= intensity × rangeFade × elevFade ×
shadowAtten) plus `invFootprintSq`/`invCoreSq`/`cutoffSq`. The shader's per-beam work is now a 2D
subtract, a dot, a squared-distance reject and two exps — **with the reject first rather than
last**, and no sqrt anywhere. `intensity` is still carried, unread by the shader, purely so the
CPU top-K eviction ranks on exactly the quantity it did before (ranking stability is load-bearing —
see the flicker history at the insertion site). Entries that fail the elevation/range/ground-hit
tests keep their slot with `weight = 0` rather than being skipped, so top-K membership stays a
function of intensity alone and doesn't churn frame to frame.

---

## Subsystem: Photometry / Shader Constants

Photometry values are **runtime members** on `SatelliteSim`, synced each frame to `SatFlarePC` —
except `brightnessScale`/`mirrorBoost`, which the reflectance model in `sat_orbit.comp` reads from
`GpuSatTypeHeader` at the front of `satTypeBuf`. They are persisted in `settings.json` and adjustable in the settings window.

| Member | Default | Description |
|--------|---------|-------------|
| `brightnessScale` | 1.0 | global flux multiplier |
| `daySuppression` | 500.0 | sky background suppression ratio (sun) |
| `mirrorBoost` | 300.0 | mirror peak multiplier (MIRROR_BOOST) |
| `visThresh` | 0.0 | visibility cull threshold |
| `highlightFlare` | 0.05 | fixed flare for highlight/census mode |
| `moonSuppression` | 4.0 | sky background suppression ratio (moon) |
| `lightPollutionGain` | 1.0 | multiplies the light-pollution dome at its source — see "Subsystem: Light Pollution Dome" |
| `extinctionCoeff` | 0.079 | sea-level zenith extinction, magnitudes — see "Subsystem: Atmospheric Extinction" |

`effectFlare = flare / (1 + (dayBright × daySuppression + moonBright × moonSuppression) × atmFrac)`,
then `×= extinction` (line of sight), then `×= (1 − domeVal × 0.85)` (light pollution)
`magnitude = kMagRef - 2.5 × log10(effectFlare / kMagRefFlare)` where `kMagRef=6.0`, `kMagRefFlare=0.008`

**Point-source appearance (2026-09-23) — one model for satellites, stars and planets.**
`shaders/include/point_style.glsl` (`pointPsf(mag)`, CPU mirror `SatelliteSim::pointPsf`) maps an
apparent magnitude — AFTER every suppression and extinction term, i.e. as drawn — to a Gaussian
PSF: displayed flux `D = 10^(-0.4·γ·(m − refMag))` minus its value at `limitMag` (so a point fades
to exactly nothing there), drawn with sigma `sigmaPx` and peak D while D ≤ 1; brighter points keep
peak 1 and widen (`σ = sigmaPx·√D`, flux-conserving) up to `sigmaMaxPx`. Sprite edge =
`2·(3σ + 1)` px (≥ ~5 px, which also keeps the S2a sub-pixel flicker fix). Each shader converts its
own record units to a magnitude first — satellites keep `effectFlare` (`satFlareToMag`: the
flare_source bloom, picking and the UI read it), stars/planets `10^(-0.4 m)` (`relFluxToMag`).
Users: `sat_point.frag` + `sat_flare.comp` (sprite size, and a zero record past `limitMag` so faint
satellites cost no fill; highlight mode too), `star_point.frag` + `updateStars()`/`updatePlanets()`
(size every frame). Parameters: the five "Point ..." Photometry sliders (`pointRefMag` 1.5,
`pointGamma` 0.8, `pointLimitMag` 8.0, `pointSigmaPx` 0.45, `pointSigmaMaxPx` 6.0, persisted under
`photometry.point_*`) → `GpuPointStyle` → `pointStyleBuf`, a host-coherent UBO rewritten every frame,
bound as `descSet` binding 11 and `starDescLayout` binding 5. Until this, satellites (sigmoid core,
log-sized sprite with a range term) and stars (sqrt core, sqrt-sized sprite) had separately tuned
curves: a mag-3 satellite drew about as bright as a mag −2 star (Jupiter), a mag-5 satellite ~13× a
mag-5 star. Not unified yet: the bloom/corona (`flare_source`) exists for satellites and the Sun only.

**Bloom and glare (2026-09-24).** The bloom (`flare_source` → `flare_blur` → `flare_composite`) is a
quarter-resolution BROAD glow: a narrow separable Gaussian plus a wide one (σ 10 texels, "Flare
streak" scales it), exactly round, final result back in `flareSourceImg` (four dispatches — see
`flare_blur.comp`'s header). Its last pass was a six-spoke, then a 16-direction streak: a bright point
came out as a dotted starfish that stopped dead where the taps ran out. The composite's ceiling is a
soft knee on luminance (unchanged below 0.45, exponential approach to 0.8); it was a hard per-channel
`min(c, 0.8)`, which flattened overlapping glows into white plateaus with sharp edges.
The sharp part is the glare (`include/glare.glsl`): a full-resolution point sprite past
`glareThreshold` in the bloom's log response — a glint core, a tight halo, and `glareSpikes` spikes of
their own length and brightness, each a line of constant pixel width (anti-aliased at any radius)
fading along its length as (1 − r/len)^`glareFalloff`, windowed smoothly to zero at the sprite edge so
a crowd of glares sums softly. The first cut used the sun corona's angular noise (hundreds of rays,
an aliased centre) and a blue anamorphic streak the sun doesn't have; both are gone.
- Satellites: `glare.vert/.frag`, one sprite per listed satellite, flare source descriptor set,
  occlusion tested at the source's screen position.
- Mesh glints: `mesh_bloom.frag` writes each texel's mesh light in effectFlare units into the flare
  source's ALPHA (per-instance `GpuMeshInstance::glareNorm` = the sprite's effectFlare per unit of
  bloom seed; satellite sprites write alpha 0), capped at 1e4 per texel (glare saturates at 256; a
  Reflect mirror in its beam overflowed the RGBA16F target to inf, so its glint's position was NaN
  and the Sun in the mirror never glared). Which light may glare: only SUN-LIKE surface brightness
  (π·L/E 150 → 1500: the Sun in a mirror ~4e4, an OSR radiator ~2e3; a rough-metal edge, even grazing,
  < 50). While the mesh is still point-like (below `kGlarePointPx` = 12 px, gone by 36) its glare is
  the SPRITE's, at its centre: the keep list's z (glare keep) goes to `sat_flare.comp`, which writes
  effectFlare × it into the visible record's last field (the pre-photometry mesh size, free by then),
  and `glare.vert` glares on max(flareIntensity, that) (2026-09-25). Until then mesh_bloom counted all
  of a point-like mesh's light, so its one glare sat on whichever few texels of the small mesh were
  brightest and jumped about until the satellite was close. The first cut let
  all of it glare: every edge and vertex of a close-up satellite flared (each a sliver of a very
  bright satellite's flux) and filled the 64 slots with ~400 px sprites, a large part of the close-up
  frame-rate drop. `glare_find.comp` lists every 5×5 local maximum past the threshold into `glintBuf`
  (`GpuGlintList`, `include/glint_list.glsl`, 64 max, its own indirect args); `glare_mesh.vert/.frag`
  draws them with the same profile.
- **Proximity (2026-09-26, `glareNearGain` / `glareNearRangeKm`):** sprite radius =
  `glareSizePx` × (0.6 + s) × `glareNearScale(range)`, which ramps 1 → `glareNearGain` over
  `glareNearRangeKm` → 0, smoothstepped — exactly 1.0 at or beyond that range, for a range of 0
  (unknown / none) and wherever `glareNearGain` ≤ 1 (the off switch). So a source hundreds of km off
  — any satellite seen as a point sprite from the ground — keeps the distance-tuned look, while the
  viewer's model and a mesh resolved in the main view, metres from the camera, spread their glare
  `glareNearGain` (3) times wider. The range comes from data the record already carries — no extra
  pass, buffer or RTT: a sprite's `GpuSatVisible::rangeM` (`glare.vert`), a glint's `glintPos.w`
  (`glare_find.comp` writes `SatelliteSim::meshGlareRangeM`, the nearest mesh it drew this frame;
  `viewer_glare_find.comp` writes `ViewerGlare::rangeM`, the viewer camera's own `viewerDist`), both
  read back through the one `glareNearScale` in `include/glare.glsl`. `GlarePC`'s old `pad0`/`pad1`
  became `nearGain`/`nearRangeM` (metres): the block is pinned at 112 B by `flareSourcePipeLayout`
  (`glare.vert` is drawn with it), so it may not grow — hence the reuse of the padding.
Photometry sliders "Glare gain / size (px) / threshold / falloff / spikes" (`photometry.glare_*`),
plus "Glare near gain / near range (km)" (`photometry.glare_near_gain`, default 3.0, and
`glare_near_range_km`, default 400). The five glare defaults are now the values tuned on screen for
satellites seen from the ground at a distance — a release build's own `settings.json` (gain 0.684,
size 29.79 px, threshold 1.765, falloff 4.678, spikes 10), not the earlier untuned 1.0/48/0.3/2.5/10
— so a fresh install starts at the shape the proximity scaling must leave alone.
  **Measured 2026-09-26** (`harness_runs\nearcheck2`, the `viewer_glare.satcmd` g20/g21/g22 A/B,
  `imgtools` at threshold 2 on a 360×280 crop of the viewer — the only place this view draws a glint):
  gain 3 vs gain 1 changes 7.1 % of that crop (bounding box 363×378 px around the glint) against 1.5 %
  (162×150) for glare on-vs-off at gain 1, i.e. a ring ~4.6× the disc it surrounds → a radius ratio of
  ~2.4, and visibly a whole burst instead of a compact star. Over glare-off, a 120×120 window on the
  glint reads +17.3 mean luminance at gain 3, +3.8 at gain 1, +11.8 for the pre-proximity build
  (`viewer_glare_final2`, which has no `glareNearScale` at all) — so the near case restores something
  like the old on-screen size while the off case is the new release shape, ~1/3 of it. Nothing outside
  the glints moves: the g10 viewer crop and a 300×300 main-view sky crop are byte-identical
  (`mean_abs` 0.0) between the two builds with the low threshold both on and off. Two whole-frame diffs
  of the same pair are *not* comparable — the sun-pillar beam and the status bar's fps text change
  frame to frame — so A/B the crop.

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

**S2c isotropic floor (RELEASE_v1_1_PLAN.md, session 30):** `elevFalloff` alone bottoms out at
~0.26 at zenith, so no `kPollutionMaxDim` could ever dim a straight-overhead target — satellite,
star, or the Milky Way — by more than ~26%, regardless of how bright the city or how high
`lightPollutionGain` went. This is why the Milky Way stayed visible near cities: it's a large,
mostly-high-in-the-sky feature living almost entirely in the region `elevFalloff` can't reach. Real
urban skyglow raises zenith brightness far more (Bortle 8 zenith ≈ 50× Bortle 1) via isotropically
scattered light, not just the horizon-hugging direct glow `elevFalloff` models. Fix, applied
identically at all four consumers (`sat_flare.comp`, `sat_sky.frag`'s Milky Way, `cloud_march.comp`'s
aurora, `updateStars()`) — `beamDomeVal`/beam-glow dome copies are deliberately left alone, since a
Reflect-Orbital beam flash is a genuinely horizon-hugging point source, not city skyglow:
```
domeVal = clamp(domeAz * (kIsotropicFrac + (1 - kIsotropicFrac) * elevFalloff), 0, 1)
```
`kIsotropicFrac = 0.4` (hand-duplicated at each site, same convention as `elevFalloff` itself).
Horizon behaviour (`elevFalloff≈1`) is unchanged; zenith now floors at `domeAz * kIsotropicFrac`
instead of `domeAz * 0.26`. Expect `lightPollutionGain` to need re-tuning after this — it now reaches
brightness levels near cities it structurally could not reach before.

**Not built:** the elevation falloff shape is a fixed analytic curve, not itself sampled/measured —
a true 2D (azimuth × elevation) dome would need real atmospheric-scattering-height modeling, judged
not worth the complexity over the fixed-curve approximation.

## Subsystem: Atmospheric Extinction

Session 26 follow-up: the light-pollution dome's `elevFalloff` was, until this was added, the
*only* term anywhere that varied a star's or satellite's brightness by its own viewing elevation —
there was no real horizon-dimming baseline, which is part of why the dome's directional noise (see
above) read as unsubtle: nothing else was smoothly dimming things toward the horizon for it to
modulate on top of.

**Formula (2026-09-23) — the air on the actual line of sight**, one function for every consumer:
`shaders/include/atmosphere.glsl` (`atmExtinctionMag(p, d, L, k)`) and its C++ mirror in
`SatPhotometry.cpp` (`updateStars()`/`updatePlanets()`). Two exponential components — molecular
(8 km scale height, ozone folded in) and aerosol haze (1.2 km) — splitting the sea-level zenith
extinction k 60/40. Each column is the Chapman function, `Ch ≈ sqrt(πx/2)·erfcx(sqrt(x/2)·cos χ)`
(x = r/H; `erfcx` by a one-parameter fit, 0.33% max error), with the tangent-point form for rays
that dip before climbing out and a segment form for finite targets (a satellite at its range, a
planet at its distance; a satellite silhouetted against Earth from orbit gets only the segment that
reaches it). `SatModelTool --selftest` checks it against a brute-force ray integral: 0.3% over
observers from sea level to 400 km, elevations +90° to -19°. Consumers: `sat_flare.comp`
(satellites), stars, planets, `sat_sky.frag` (Milky Way, zodiacal light, and the aurora/Milky Way
ocean reflections — from the water along the reflected ray), `cloud_march.comp` (aurora, beam ray).

It is **absolute** — the zenith from sea level loses k magnitudes, orbit loses nothing — where the
old one was relative to the zenith. It replaced Kasten & Young's sea-level airmass times
`exp(-tangentAlt / 80 km)`: a 10× too slow thinning with height (a 4 km mountain kept 95% of
sea-level extinction, really ~35%; an aircraft at 10 km 88%, really ~15%). `rayTangentAltM`, the
gate it needed, is gone. At k = 0.25: sea level 0.25 / 1.41 / 14.4 mag (zenith / 10° / horizon —
the horizon is above Kasten & Young's ~9.5 because low haze stacks up along grazing paths), 4 km
0.09 / 0.53 / 3.5, 10 km 0.04 / 0.24 / 1.5.

Satellites' day/moon sky dimming (`sat_flare.comp` `atmFrac`) now uses the stars' altitude gate —
full through the flyable atmosphere, fading over 40-100 km — instead of `exp(-h / 80 km)`, so stars
and satellites in the same sky are dimmed alike.

**Tunable:** `extinctionCoeff` — the sea-level ZENITH extinction in magnitudes (V band, clear sky:
~0.2-0.3; the user-tuned default is 0.079), settings slider "Extinction". Reuses `SatFlarePC`'s
`pad2` slot (the one freed by `lightPollution`'s move to `lightDomeBuf`); stars read the same C++
member directly.

`MIRROR_BOOST = 300` — peak multiplier for near-perfect mirror alignment. `mirrorExp = max(specExp0 × 300, 8000)` gives sub-degree angular width (matches solar disc ~0.26°).

---

## Subsystem: GpuSatInput (deleted 2026-09-22)

The 80-byte `sat_orbit.comp` → `sat_flare.comp` hand-off record no longer exists — see "Lighting
overhaul Phase 1" under "Subsystem: GPU Orbital Pipeline". Its job is done by the pre-photometry
`GpuSatVisible` encoding. Below-horizon and disabled satellites are simply never appended to the
compact list (Phase 1b); `sat_orbit.comp` also dropped the per-satellite `asin` that existed only
to feed `sat_flare.comp`'s duplicate horizon cull.

---

## Subsystem: Sky Glow SSBO

**Air toward the satellite (review 6):** `sat_sky.frag`'s glow loop weights each bin by the smaller of
the air along the pixel's ray and the air between the eye and the bin's direction (from the eye's height,
or the tangent height of a path below the local horizontal; Chapman airmass capped at ~35). From 40+ km
the pixel-ray weight alone lit the whole limb around every glaring satellite. Unchanged from the ground.

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

## Subsystem: The Moon as a body (2026-09-30)

`updatePositions` computes the Moon's geocentric ECI position (`satphot::moonGeoEciAt`: Meeus ch. 47's main
periodic terms — 14 in longitude, 7 in latitude, 10 in distance; ~0.05 deg, eclipse times within minutes) and
from it, per frame: the TOPOCENTRIC direction (`moonDirENU`, up to ~1 deg of parallax from the ground, far
more in deep space), the true angular radius x "Moon size (x real)" (`moonSizeScale`, slot 220, key
`clouds.moon_size`; the old disc was a fixed 3x at infinity), the Earth-centred position in the observer's ENU
(km) and the eclipse state — all into `cloud.moonCenter` / `cloud.moonMisc` (UBO 784 -> 816).
- **Tidally locked**: the texture frame's z axis points at the EARTH'S CENTRE (it pointed at the observer, so
  the face never turned); the far side (from space) is the near-side image flattened toward its mean.
- **Its light follows the PHASE LAW (review 16):** `moonDirENU.w` is the Moon's light relative to full (Allen:
  V = V0 + 0.026 a + 4e-9 a^4, a = the phase angle; quarter 0.09, a 32% crescent 0.04), read by every moonlit term
  (clouds, ground, sky, halo, star/satellite dimming). It was the lit FRACTION, linear: a crescent lit the clouds and
  the sky at a third of full. The disc's own shading recomputes the fraction; harness `state` gives both (`illum`,
  `brightness_vs_full`). "Moon size (x real)" defaults to 2.5 (the true 0.5 deg is a few pixels at a wide FOV);
  within 1.5-4 deg of the Sun the drawn disc eases back to its TRUE size (`moonAngR`), so a solar eclipse's
  contacts, totality and annularity are the real ones (at 2.5x the Moon swallowed the Sun long before contact).
- **The eye stops at 100,000 km** above sea level (`kMaxObsHeightM`: Q/E, the altitude scroll, the follow offset;
  review 16 — boost reached the Moon).
- **Lunar eclipses** per surface point: the Earth's disc (enlarged 1/85 for the atmosphere, Danjon) against the
  Sun's (seen from that point, with the Sun's 0.15-deg parallax from the Moon), penumbra -> umbra, and the
  umbra's red refracted light.
- **The Sun's angular radius is its TRUE one** (`sunAngRTrue` from `satphot::sunDistAuAt`, 0.2624 deg in July ..
  0.2711 in January; `cloud.taaJitter.w`, `kSunAngR` in sat_sky.frag): it decides total vs annular. It was a
  fixed 0.2667 deg.
- **Solar eclipses**: near the Moon the Sun disc takes its TRUE radius (it is drawn ~2x large for the glare
  otherwise) and is hidden where the Moon is; glare, halo and the sky dim with the Sun the observer sees
  (`moonMisc.x`, CPU `discOverlapFrac`); a corona at totality; the Moon's shadow on the ground per pixel
  (`moonSunVisible` on `sunDiscVis` for land, `directSun` for the sea) and uniformly on the clouds' key light
  (per-sample would risk the v2 march's register cliff). **Everything sunlit must take the eclipse factor**
  (2026-10-03, found at the real 2037-07-13 totality, where the sky stayed bright): the land's direct sun (it
  reached only the sea), the terrain's sky light (`skyAmbientTerrain`), the clouds' sky light (the march's zenith
  cache), the fog/dust march's `sunF`/`skyF` (the desert dust stayed sunlit), the Sun's bloom seed
  (`FlareSourcePC::sunRefIntensity` only — `SatFlarePC`'s is every satellite's soft ceiling, 0 would zero them)
  and the Sun's lens flare. An annular eclipse falls out of the geometry.
- **The Moon's shadow per point (2026-10-03, `include/eclipse.glsl`):** `eclipseSunVis(pKm, sunD, blurKm)` = the
  Sun's visible share from any point (equal-disc lens area between the umbra edge and first contact; CPU mirror
  `eclipseSunVisCpu`), and `eclipseSkyLight` = that blurred over `kEclipseSkyBlurKm` (180) x the colour of light that
  crossed `kEclipseSideKm` (80) of air sideways at the point's height + the ozone shell at a grazing secant (~20): the
  sky light inside the shadow, deep orange low, blue high. Used by: the sky loop per sample (direct + second scattering,
  `kEclipseMultiScatter` 2 in sat_sky.frag; also the sea's reflection march), land direct sun and sky light, the cloud
  march's key light per sample and its zenith cache, the fog/dust march, the far cloud layer. Everything is gated on
  `cloud.moonMisc.y` (uniform), so outside an eclipse it is a branch; the cloud march stays at 128 registers. The
  orange horizon of totality comes out of this (lit air beyond the umbra seen through the low air, plus the reddened
  sideways light) — no colour grading. `skyExposure()` adapts to the observer's sky light (`moonEclipseSkyObs`, dayness
  x it), so totality is shown at about twilight's exposure (stars come out). Harness `state` moon block:
  `sky_light_frac`, `sky_exposure`. `scripts/eclipse_visuals.satcmd`.
- **The corona** (`solarCoronaAt`, sat_sky.frag, only near totality within `kCoronaMaxR` 9 R of the Sun): fibres along
  dipole field lines to a source surface at 2.5 R (radial beyond), each point mapped to its line's footpoint and the
  legacy cirrus volume (`cloudNoiseTex`) read on that circle — fine across the lines, stretched along them; footpoints
  mirrored about the solar equator (taken per hemisphere they jumped there: a seam), where a streamer stalk is drawn;
  coarse reads for streamers; Baumbach's radial profile; H-alpha pink chromosphere and prominences at the limb. The
  solar axis is the ecliptic pole. `kCoronaGain` 0.4.
- **The Moon writes its distance into the unified depth**, so stars and planets behind it are depth-occluded
  and satellites (nearer) draw in front; star_point.vert's cull takes the real radius from
  `PointDrawPC::moonDirENU.w`.
- **Real eclipses at their real UTC times** (2026-10-03): the 2037-01-31 total lunar eclipse greatest at 13:59:48
  (real ~14:01), the 2037-07-13 total solar eclipse greatest at 02:42:56 at 21.75 S 138.31 E (Moon/Sun 1.043; real
  magnitude ~1.04). Harness `eclipse solar|lunar` finds the next one after the current time
  (`SatelliteSim::findEclipse`, hourly scan + golden section): solar when a partial is seen anywhere on Earth
  (separation < the Moon's parallax + both radii - the Sun's parallax), lunar when umbral; it used to need 0.55 /
  0.6 deg and skipped the July 2037 eclipse (0.68 deg). It places the observer under the Moon (lunar) or where the
  shadow axis meets the Earth (solar; under its nearest point for a partial-only one). `state` reports the moon's
  `sep_from_sun_deg`, `sun_seen_frac`, `drawn_radius_deg`, `sun_radius_deg`, `dist_km`.
  `scripts/eclipses_2037.satcmd` (the two 2037 eclipses from Alice Springs, Charleville, Sydney), `scripts/moon.satcmd`.
  Sun and Moon are of date; stars, the Milky Way and the planets are J2000 (~0.5 deg of precession apart in 2037). SKY_ENV probes use the main observer's
  `moonCenter`, so the Moon's face in reflections is approximate.

## Subsystem: Planets

See `PLANETS_PLAN.md` for the session log and forward-looking next-steps list (in-app QA still
outstanding, known simplifications, ring rendering, attribution follow-up) — this section is the
architecture/design writeup only.

Session 30. Mercury, Venus, Mars, Jupiter, Saturn, Uranus (`enum PlanetId`, `kPlanetCount = 6` —
Neptune excluded, never naked-eye at ~mag 7.8) with real Keplerian-approximation orbital positions,
rendered as clickable points of light. Deliberately **not** built on the satellite orbital-compute
pipeline (`GpuSatOrbit`/`sat_orbit.comp`) — that solves near-field Earth-relative geometry (shadow,
attitude, specular surfaces) planets don't have. Instead: closed-form CPU math in the sun/moon
pattern (`updatePositions()`), rendered through the star pipeline's shape (direction + magnitude-
driven brightness/size, no near-field 3D position needed at render time).

**Ephemeris** (`SatelliteSim.cpp`, top-of-file constants block, `keplerEclipticPos()`): low-precision
Keplerian elements + linear centurial rates (JPL/Standish, valid 1800-2050 —
https://ssd.jpl.nasa.gov/planets/approx_pos.html), one `KeplerElements` row each for Earth
(`kEarthElements`, the table's EM Bary row) and the six planets (`kPlanetElements[]`). Computed every
frame in `updatePositions()`, right after the Sun/Moon block, using the same `Tcent` (Julian
centuries since J2000, derived from the same `dJ2000` the Sun calc already computes) and the same
`epsR` obliquity rotation — no separate time base. Standard Newton-Raphson Kepler-equation solve +
3-1-3 (ω,i,Ω) orbital-plane-to-ecliptic rotation; geocentric vector = `helio_planet - helio_earth`.
Results land in `planetStates[kPlanetCount]` (`PlanetState`: `eciDir`/`distanceAU`/`sunDistAU`/
`phaseAngleDeg`), a plain ephemeris record — distinct from the render-ready `GpuSatVisible` entries
`updatePlanets()` derives from it each frame.

**The Moon** is not on this Keplerian path: since 2026-10-03 it is `satphot::moonGeoEciAt` (Meeus ch. 47's
periodic series — see "The Moon as a body"). Its earlier two-body fit (and before that a circular orbit
with a hand-calibrated phase) put eclipses hours late.

**Brightness** (`updatePlanets()`, mirrors `updateStars()`): apparent magnitude via
`planetApparentMagnitude()` — Paul Schlyter's standard formulas
(stjarnhimlen.se/comp/ppcomp.html), `V = V0 + 5*log10(r*Δ) + phase-angle polynomial`. Saturn's ring
brightness is deliberately omitted (needs Saturnicentric ring-plane geometry, not just phase angle —
accepted simplification, dims slightly near ring-plane-open oppositions). Converted to
`rawIntensity = 10^(-V/2.5)` — **the same convention `initStars()` uses** — so a planet's brightness
runs through the exact same suppression chain stars already have (day/moon/pollution-dome/
extinction), hand-duplicated into `updatePlanets()` per this codebase's established per-consumer-
duplication convention for that formula (see "Subsystem: Light Pollution Dome" above). Its point
is drawn by the shared point-source model (see "Point-source appearance" under Photometry), so a
planet reads at the same visual weight as an equally bright star or satellite. Color (`kPlanetColor[kPlanetCount]`, same-day follow-up):
hand-picked approximate true colors, not computed — planets have no B-V spectral index to derive
one from the way stars do. Mars is the one that actually reads as visibly colored at naked-eye
scale (rust/salmon); the others stay close to near-white/pale by design, matching their real subtle
cloud-top/regolith colors. `star_point.frag`'s desaturation (bright = full tint, faint = fades
toward white, keyed on the drawn peak) applies to planets unchanged.

**Rendering**: a second tiny host-mapped `planetBuf` (`GpuSatVisible`-shaped, 6 entries) + a second
descriptor set (`planetDescSet`, reusing `starDescLayout`/`starDescPool`'s shape via its own tiny
`planetDescPool` — `starDescPool` itself is sized `maxSets=1`) — but the **same** `starPipeline`/
`starPipeLayout`/shaders (`star_point.vert`/`.frag`), just a second `vkCmdDraw` in `recordDraw()`
right after the star draw. Reusing the pipeline object means `onResize()`'s existing
`createStarPipeline()` recreation covers planets for free — no separate resize handling needed.
One real shader difference was necessary: `star_point.vert`'s atmospheric scintillation/twinkle is
physically wrong for planets (small resolved discs, not point sources) — gated behind a new
`noTwinkle` field, set to 1 only on the planet draw's own copy of the push constant. (Originally
`SatDrawPC.noTwinkle` at offset 164; now `PointDrawPC.noTwinkle` at offset 76 after the
128-byte push-constant split — see the **PointDrawPC** entry under "Subsystem: GPU Orbital
Pipeline".)
The pre-existing Moon-disc occlusion cull in the same shader stays active for planets unchanged
(correct — a planet behind the Moon's disc should still be culled).

**Picking**: `pickPlanetAt()` is cheaper than `pickSatelliteAt()` — `planetBuf` is already
HOST_VISIBLE/COHERENT (`updatePlanets()` writes it directly from the CPU), so unlike satellites'
device-local `satVisibleBuf` there's no staging-buffer copy at all, just a loop over 6 entries using
the existing `projectSkyDirToScreen()`. `selectedPlanetIndex` (mutually exclusive with
`selectedSatIndex` — selecting one clears the other) is tried first (planet priority on an exact
overlap) at both click sites (`SatelliteSimUI.cpp`'s mouse-click handler and `KB_SELECT_SAT`'s
center-screen equivalent). `formatSelectedPlanetInfo()` fills `planetInfoLine[]` (name/magnitude/
distance/phase) — unlike `formatSelectedSatInfo()` (called only when the selection changes, since a
satellite's orbital elements are static), this is re-called **every frame** the selection is active
(right after `updatePlanets()` in `recordCompute()`), since a planet's distance/phase/magnitude
changes continuously. `buildSelectedSatPanel()` branches on `isPlanet` to pick its data source
(`planetBuf`'s own mapped memory directly for planets — never stale, vs. satellites' one-frame-stale
`lastPickedSkyDir` GPU round-trip) and info-line array, then shares the rest of the panel/reticule
rendering unchanged.

**Settings**: `showPlanets` (global) + `planetEnabled[kPlanetCount]` (per-planet), UI in
`buildSettingsConstellationsTab()` reusing the exact ON/OFF row pattern the constellation list
already uses. Persisted as `j["planets"]` (`{show_planets, list:[{name,enabled}]}`) in
`saveSettings()`/`loadSettings()`, same shape and same ungated (non-schema-versioned) treatment as
`j["constellations"]` — not a graphics-tuning value a schema mismatch needs to guard against.

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

**In-app GPU timestamp queries** (`VulkanContext`): a `VK_QUERY_TYPE_TIMESTAMP` pool whose slot
count has changed several times as passes came and went — `VulkanContext::kTimestampCount` carries
the authoritative slot table and is the only place that mapping is documented. `updateGpuTimingStats`,
`kPerfLabels[]` and `savePerfSnapshot()`'s JSON keys all mirror it and must be updated together.
Single frame in flight, so results are resolved in `App::drawFrame` right after the fence wait —
no stall. Slot layout is a shared contract: App.cpp writes 0 (frame start), 5 (satellite+star draw
done), 6 (UI overlay done); `SatelliteSim` writes 1-3 in `recordCompute` (cloud march / orbit
compute / flare compute done) and 4 in `recordDraw` (sky background draw done — this is what
isolates the fullscreen atmosphere/terrain/ocean shader's own cost from the satellite/star point
draws that follow it in the same render pass; they used to be one fused bucket).
`SatelliteSim::updateGpuTimingStats()` EMA-smooths the six deltas into `gpuMsSmoothed[6]`,
displayed in Settings → Performance → "GPU FRAME (ms)" (the Display tab until 2026-10-03) (one-frame-stale, same pattern as
`peakMagnitude`).

**CPU frame timing** (`CpuBucket` / `cpuMsRaw[]` / `cpuMsSmoothed[]` / `beginCpuFrameTiming()`,
2026-08-10): the counterpart to the GPU timestamp buckets, displayed in Settings → Performance →
"CPU FRAME (ms)" and logged as `cpu_timing_ms` (snapshots) / `knockout_sweep.baseline_cpu`
(sweeps). Buckets: `build_ui`, `update_positions`, `beam_readback`, `update_stars`,
`light_pollution_dome`, `update_planets`, plus a derived `other` = wall clock − GPU total −
everything measured (present/vsync wait, driver submit, App-side work, and any CPU block without a
bucket yet). **A large `other` is a finding, not a gap to hide** — it says the cost is somewhere
this table doesn't look.

Built because the GPU work above succeeded: once Medium's Anchorage GPU frame reached 15.5 ms, the
Release wall clock was 18.3 ms, and the ~2.8 ms remainder was *near-identical at Planetarium*
(2.73 ms against a 6.0 ms GPU frame — 31% of that frame). Fixed per-frame cost that doesn't scale
with rendering load is exactly what the GPU buckets exist to expose, and nothing equivalent existed
on the CPU side, so that number was opaque and unshrinkable without guessing.

Timers are scoped (`SatelliteSim::CpuTimer`, an RAII adder — it ACCUMULATES rather than assigns, so
one bucket can be timed at several sites or inside a loop). `beginCpuFrameTiming()` must stay the
**first statement in `buildUI()`**, the first sim entry point of a frame: it publishes the previous
*complete* frame and clears the accumulator, so it can never publish a half-filled one. The
resulting one-frame staleness deliberately matches `gpuMsRaw[]`'s, which is what lets a sweep step
sample a CPU frame and a GPU frame from the same moment instead of one lagging the other.

**Perf knockout toggles**: `debugDisableMask` (uint32) is a profiling-only bitmask. It rides in the
CloudParams UBO as `cloud.dbgDisableMask` (read by `sat_sky.frag` and `cloud_march.comp`), plus a
copy in `PointDrawPC` for `sat_point.frag`'s bit 4096 — all pushed from the single
`SatelliteSim::debugDisableMask` member each frame. Toggles in Settings → Performance →
"KNOCKOUTS" ("Skip" = knocked out) (19 as of 2026-09-23, driven by the single `kDebugToggles` table at the top of
`SatelliteSimUI.cpp` — bit, display label, and stable JSON key per row; adding a row there adds a
checkbox AND a sweep step for free) each disable one shader block or dispatch — each with a mathematically-safe
zero/no-op fallback (e.g. terrain-skip leaves `tHit=-1`, the same value the "no hit" path already
produces) or, for the two producer-side bits, a "reproduce pre-feature behaviour" fallback. Default
mask 0 is bit-identical to normal rendering. Use this to isolate one block's real GPU cost via
before/after `gpuMsSmoothed` deltas, without a GPU capture tool — bit assignments: 1=terrain,
2=atmosphere, 4=sunOD (`optDepth`, called from 4 sites — zeroing it there is a single early-return
in the function itself, not 4 separate call-site edits), 8=oceanRefl, 16=airglowRed, 32=aurora
curtain, 64=cloud self-shadow cone, 128=Reflect-Orbital beams (both `cloud_march.comp`'s
volumetric term and `sat_sky.frag`'s ground-spot term), 256=cloud shadow (per-pixel, in
`cloud_march.comp`), 512=`beam_self_march.comp` DISPATCH itself (producer-side; repurposed
2026-08-09 from the now-retired `beam_cloud_block.comp`'s identical bit), 1024=scene depth pass
DISPATCH itself (producer-side — the big one: skipping it reverts the entire shared-depth
architecture to pre-unification occlusion behaviour), 2048=fog + dust (clouds v2; was v1's fog layer), 4096=satellite point
cloud occlusion (`sat_point.frag` — added 2026-08-09 to isolate a reported perf question, see
`BEAM_CLOUD_PLAN.md`; ruled out as the cause, kept as a real diagnostic),
8192=Reflect-Orbital beam POINTING-RAY loop (`cloud_march.comp`'s per-pixel loop in `main()`),
16384=`cirrusMarchCS`, 32768=`cloudMarchCS` (the volumetric low/mid march itself),
65536=`sat_sky.frag`'s 64-bin satellite sky-glow loop, 131072=**beam tile cull OFF** (not a feature
knockout — an optimization A/B; see "Beam pointing-ray tile culling" below), 1048576=satellite
part occlusion (`sat_orbit.comp`, geometry-model types; via `GpuSatTypeHeader::occlusionOn`),
4194304=the water map in the shared height function (terrain v2 P2, `tdDemAt`; also the cost A/B),
262144=**Potato sky** (swap `skyBgPipeline` → `skyBgMinimalPipeline`), 524288=**SKY_LITE sky**
(swap → `skyBgLitePipeline`) — see "Subsystem: Weak-Hardware Sky Tiers". These last two are
pipeline swaps, not in-shader branches; they're set by the Potato / Planetarium presets and are
NOT part of the `kDebugToggles` sweep.

**Bits 8192-65536 were added 2026-08-10** for the Anchorage worst-case profiling session, and all
four cover blocks that previously had NO knockout, so their cost was permanently invisible inside a
lumped bucket. Two of them (8192, 65536) additionally had no quality slider and **no preset reach**
— `applyGraphicsPreset` could not turn them off at any tier, so Planetarium paid for them in full.
Bit 8192 in particular closes the gap this document already flagged under "Subsystem:
Reflect-Orbital Beam Cloud Occlusion" ("knockout bit 128 gates … but NOT `cloud_march.comp`'s
per-pixel ray loop … flagged here so a future profiling session doesn't assume bit 128 isolates the
full beam rendering cost").

**Automated knockout sweep** (Settings → Performance → "Run knockout sweep", `startKnockoutSweep`/
`updateKnockoutSweep` in `SatelliteSimUI.cpp`): walks the whole `kDebugToggles` table on its own —
baseline first, then one step per bit — holding each mask for `kSweepSettleFrames` (6) discarded
frames then averaging `gpuMsRaw[]` over `kSweepSampleFrames` (24), and appends ONE
`"record_kind": "knockout_sweep"` record carrying every step's full bucket breakdown plus its
`cost_ms` delta against the baseline. ~15 s for the whole table.

Two things make it trustworthy where a hand capture isn't. It reads `gpuMsRaw[]`, **not**
`gpuMsSmoothed[]` — the HUD's EMA (α=0.1) takes ~40 frames to settle, so a hand capture taken soon
after flipping a checkbox silently reads a blend of two configurations. And it **forces
`timePaused` for its duration** (restoring the user's value after), so all 18 steps measure the same
frame; without that, satellites move, beams re-target and clouds drift across a multi-second sweep
and the per-bit deltas mix real cost with scene change. The record's top-level `gpu_timing_ms` is
overwritten with the sweep's own baseline window rather than the EMA, which at write time is still
decaying out of the last knockout step. `cost_ms` is deliberately **not** clamped at zero — a
knockout can legitimately come out negative (noise, or a skip that makes a later pass do more work
because nothing occludes it any more, bit 1024 being the standing example), and that is information.

`analyze_profile.py`'s `print_sweep_report()` prints each sweep as a ranked cost table and, per row,
which bucket actually moved most — measured rather than assumed, since a bit can sit in a different
pass than expected (128 spans two shaders; 1024 is a producer whose skip shows up downstream).

**`perf_profiles/profile_log.jsonl`**: the "Save snapshot" button (Performance tab, RECORD) appends one JSON
record per press — GPU timing breakdown, resolution, observer lat/lon/altitude, sim time, active
knockout mask, GPU device name, quality settings, graphics preset name, and (2026-08-10) a `beams`
block: `active_count`/`ground_spot_count` and their capacities, plus `show_beam_rays`. Beam count is
a first-order driver of `cloud_march`'s cost (its pointing-ray loop iterates
`min(beamCount, 2048)` times per half-res pixel) and varies enormously by observer location, so
without it the Anchorage captures were not interpretable. JSON Lines (not a JSON array) so the log
grows by simple appending across sessions/restarts. Every record now carries `record_kind`
(`"snapshot"` or `"knockout_sweep"`). `SatelliteSim::savePerfSnapshot()` and
`buildPerfSnapshotJson()`/`appendPerfRecord()`, which the sweep shares so the two record kinds
can't drift apart.

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

## Subsystem: Cloud Shadows

Marched **per pixel** inside `cloud_march.comp` (`cloudGroundShadow`), sunward from the terrain hit
point `scene_depth.comp` supplies, and delivered in `cloudTargetB.a` — the channel freed by
deleting `tEnterCombined`. `sat_sky.frag` consumes it as a single `directSun *= cloudB.a`.
Gated on `tScene < kNoSurfaceT`, so sky pixels pay nothing. Knockout bit 256.

This replaced `cloud_shadow.comp`, a fixed 128x128 observer-centred tangent-plane grid, which is
worth understanding because the failure modes were structural rather than tuning:

| | 128² grid | per-pixel |
|---|---|---|
| Resolution | 1250 m/texel uniformly at the 80 km default | screen-space; metres near camera, coarsens with distance |
| From altitude | observer-centred at fixed world extent, so it covered less and less of the visible ground | follows the view ray |
| Range | hard cutoff at `cloudShadowRangeM` (which had already caused a real beam bug, worked around via `blockOpacity`) | none |
| Swimming | needed `computeCloudShadowSnap()` + a residual subtracted by every consumer | nothing to snap — the value is a function of the world point being shaded, not the camera |

Deleted with it: the image/view/sampler/descriptor set/pipeline, `CloudShadowPC`, the snapping
block, two `SatDrawPC` fields, the "Cloud shadow range (m)" slider and its settings key, and the
pass's timestamp bucket. Same 12 steps and the same `3e-3` density→optical-depth constant the grid
used, so brightness is comparable. The march phase is jittered off `noiseTex` because adjacent
half-res texels can map to ground points kilometres apart at grazing angles.

---

## Subsystem: Sky TAA — temporal anti-aliasing of the background (2026-09-30)

The user asked for terrain anti-aliasing; silhouettes, detail normals, far textures and city glitter all
aliased because the sky pass shades one ray per pixel. At renderScale 1 with the full sky shader
(`skyTaaWanted`; Potato/Lite and renderScale < 1 keep their paths), `recordPrePass` → `recordSkyTaa`:
1. `sat_sky.frag -DSKY_TAA` (`sat_sky_taa.frag.spv`) renders the background offscreen with a Halton (2,3)
   sub-pixel jitter (`cloud.taaJitter.xy`, applied to the ray via the interpolated direction's screen
   derivatives) into RGBA16F + its unified depth as an R32F colour (no depth attachment).
2. `sky_taa.comp` reprojects the previous RESOLVED frame exactly (camera rotation + the eye's motion at each
   pixel's depth, both from the CPU in double, in this frame's ENU; sky by rotation alone), samples it
   Catmull-Rom, rejects disocclusion against the RANGE of the 2x2 history depths (against one texel it
   rejected every silhouette pixel every frame — no anti-aliasing at all), variance-clips it in YCoCg (the
   3x3 of this frame) and blends (`display.sky_taa_weight` 0.1 still, `_moving` 0.35 past ~4 px of motion).
   Ping-pong histories in GENERAL layout.
3. The result is blitted into the swapchain; the main pass (renderPassLoad) opens with `taa_depth_restore.frag`
   writing the depth back (colour writes off), so stars, planets and satellites draw over it unjittered and
   occluded. Display tab "Temporal AA (terrain, sky, sea)", `display.sky_taa`. Cost +1.1 ms still, +0.8 panning
   (1600x900). The half-res cloud composite rides in it, so cloud edges are averaged on screen too.
- **The swapchain now has TRANSFER_DST usage** (`swapTransferDstSupported`): the renderScale < 1 prepass had
  been blitting into images without it. **The main pass's three variants share ONE dependency list**
  (`mainPassDependencies`): compatibility includes dependencies, and every draw of a load or boot frame was
  invalid against the framebuffers and pipelines made with ctx.renderPass (validation layer, 2026-09-30).
- **Temporal UPSCALING below render scale 100% (2026-10-04):** the TAA path now runs at any render scale (with the
  full sky shader). The sky pass renders into the top-left `skyTaaInExtent()` of its full-size input images
  (dynamic viewport/scissor, so a scale change recreates nothing), jittered over 16 Halton phases in INPUT pixels; the
  resolve (`sky_taa.comp`, output size) builds this frame's estimate at each output pixel as a Gaussian (sigma 0.6
  output px) of the 3x3 input samples at their jittered positions, and scales its blend weight by the nearest
  sample's Gaussian weight (`conf`); the 100% path is the old code unchanged. SkyTaaPC's spare w's carry the jitter
  and the input size. The depth restore reads the RESOLVE's full-res depth (`skyTaaRestoreSet[2]`, one per ping-pong
  side). The sky pass's detail LOD uses the OUTPUT pixel (`CloudParams::skyLodScreenH`, v1's unread maxRenderDistM).
  Cost ~1 ms over the plain stretch (the full-res resolve). The plain low-res blit stays for TAA off / SKY_LITE.
- **Point-like lights under the sky TAA (2026-10-04, user snapshots: far city lights blinking "like z-fighting").**
  Two benchmarks, run on BOTH sides of any change here (docs/HARNESS.md): the still-view flicker set
  (`harness_runs/rscale/bench4.satcmd` + `bench4.py`: Irvine horizon from 330 m, SF Bay from 1.2 km, SoCal from 1805
  km, Dallas from 3.5 km; % of pixels varying > 8 levels; TAA off reads 0%) and the climb set over clouds
  (`harness_runs/trails/r37|r38.satcmd`, tstab `rise` + `boostrise`; profile_log_1004j 37 Pacific cumulus from 2 km, 38
  Edmonton from 4.3 km). Committed state before the day: flicker 2.31 / 0.58 / 1.07 / 0.94, climb 1.60 / 0.85. Now:
  flicker 0.57 / 0.04 / 0.13 / 0.04, climb 1.65 / 0.91 (the moving frames differ from the committed ones only by
  cloud-march sampling grain, ~0.4/255 mean; checked by eye).
  - **The resolve** (`sky_taa.comp`, otherwise as committed): when NOTHING moved this frame — the CPU's exact test:
    the eye moved < 1 cm, the view did not turn, sim time <= ~1.5x; `eyeDelta.w` = 2 — the history is not clipped
    (a light smaller than a pixel is caught by only some jitter phases, and in the others the all-dark 3x3 box clipped
    its accumulated light away) and the flash rule uses the 3x3 mean without its brightest sample (a lone glint on a
    wave facet took its frame raw: white specks on the sea). Non-finite inputs are dropped (`fin`).
  - **The city lights** (`sat_sky.frag`): read at the UNJITTERED pixel only where the view is steeper than ~12
    degrees (`uvTaa0` / `taaDn`, gated on |cos| > 0.2 of the unjittered ray): the night map, the terrain contour, the
    land test and the pattern's point. Glitter points are at least an input pixel wide under the upscale, the
    glitter's presence ramps over a band of the hash, the steep-ground cut fades past a 400-1200 m footprint, `cFoot`
    takes the sphere's slope past ~100-400 m; city sprites take their hardware depth at 0.95 of their range.
  - **Tried and REVERTED, each made rendering less stable** (the user: "over-optimizing the bugfix ... made rendering
    a lot more unstable"; trails climbing over clouds): (1) the unjitter at every view angle — the tangent-plane point
    slides kilometres toward a grazing horizon (Irvine 2.3% -> 4.6%); (2) a PER-PIXEL "still" (reprojection < 0.1 px)
    widening or dropping the clip and raising the flash threshold — climbing, the far clouds move less than that while
    the layer being crossed changes: trails and stair bands (climb 1.60 -> 3.87, and two settles of one view no
    longer agreed); (3) the flash rule against the history's neighbourhood; (4) restoring the 3x3 farthest depth with
    a wider disocclusion tolerance; (5) the disocclusion tolerance widened by the 3x3 depth spread; (6) the
    footprint's |cos| floor 0.2 -> 0.03. Most of these measured well on the still views alone.
  Debugging notes: print the worst pixels' value series (lit in 1 of 8 or 16 frames = the jitter; any other period is
  something else) and A/B with the history off (`set display.sky_taa_weight 1`); `Copy-Item` keeps a file's old
  timestamp, so restoring a shader from a backup does NOT rebuild it (touch it) — identical-to-the-digit results
  across a change mean it did not build. The harness is bit-deterministic run to run. `cloud_v2_resolve.comp` writes
  "no cloud" for a non-finite result (user snapshot: black squares at the limb after a fast climb; not reproduced).
- **Presets (2026-10-04, the user):** Medium renders at 67%, Low is Medium's settings at 50% with every effect on
  (aurora, beams, fog, terrain detail); Planetarium is the "everything off" tier. Measured (RTX 3070 Ti, 1600x900, the
  user's snapshots): Low 12.3 / 13.8 / 9.9 / 7.9 ms, Medium 17.8 / 18.2 / 13.8 / 11.2 (orbit storm, anvils, Dushanbe,
  Dallas night). First runs on integrated GPUs (and CPU / virtual devices) seed **Planetarium**
  (`seedGraphicsPresetFromDevice`; discrete GPUs Medium), and the tutorial then ends with a "Graphics" card
  (`TUT_GRAPHICS`, shown only when it starts on Planetarium or Potato) saying there are no clouds and pointing at
  Settings > Display > Preset.
- **Automatic render scale (2026-10-04, `updateDynamicResolution`, Display "Automatic render scale", keys
  `display.dynamic_resolution` (off) / `dynamic_target_fps` (60) / `dynamic_min_scale` (0.5)):** keeps the GPU frame
  under 0.9 x 1000 / fps by 5% steps (down as far as a 30% fixed + 70% x scale^2 model says, after 0.75 s; up one step
  when predicted under 85% of the budget, after 1.5 s). The clouds' history is BLITTED across a resize
  (`recreateComputeScaledTargets`), so a step does not restart them. The orbit storm snapshot settled at 55% (13.8 ms).
- Run with `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` after touching any of this; the harness run's
  `app_stdout.txt` holds the messages. `scripts/sky_taa.satcmd` (set `display.sky_taa` with `true`/`false`).

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
drifted off-center, fully distorted at 50%. The sky render-target size is now `cloud.skyScreenW`/
`skyScreenH` in the CloudParams UBO (`skyLowResExtent` when the scaled prepass runs — recordPrePass
and recordDraw's Pass 1 are mutually exclusive, so one per-frame value suffices — else
`ctx.swapExtent`); any `[0,1]`-normalized UV in `sat_sky.frag` derived from `gl_FragCoord` divides
by `vec2(cloud.skyScreenW, cloud.skyScreenH)`, never an assumed constant. (A fixed-frequency noise
seed like the terrain-march jitter lookup, `gl_FragCoord.xy * (1.0/128.0)`, is fine as-is — no
total-resolution assumption baked in, not a normalized UV.) The point shaders (`sat_point.frag`/
`star_point.frag`) never render scaled, so they keep a plain `screenSizePx` (= `ctx.swapExtent`) in
`PointDrawPC`.

`screenSizePx` originally lived in `SatDrawPC` (which grew 132→144 for it); it moved to the
CloudParams UBO / `PointDrawPC` in the 128-byte push-constant split. `buildSkyDrawPC(ctx)` /
`buildPointDrawPC(ctx)` fill the two draw push constants; the CloudParams UBO fill in
`recordCompute()` sets `skyScreenW`/`H` from `renderScale`.

See `TERRAIN_PLAN.md` session 29 log for the full design writeup and the bug's root-cause
narrative.

**The half-res compute passes follow it (2026-10-04, "Clouds follow render scale", `display.clouds_follow_render_scale`,
default on):** scene depth (+ its quarter seed), the cloud march targets and clouds v2's screen images are sized by
`computeHalfExtent()` = half of the SCALED extent, recreated by `recreateComputeScaledTargets()` (onResize, and
recordCompute's check before anything is recorded when the extent changes). Every consumer already sized itself from
`imageSize`/`textureSize` or sampled by UV. The march's noise LOD keeps the 100% pixel (`cv2.motion.w` = march pixel /
100% pixel x `fp`): with the coarser footprint the storm cumulus texture filtered away. Measured (RTX 3070 Ti,
1600x900, user snapshots, full-rate march): orbit storm total 32.4 / 20.6 / 11.9 ms at 100 / 75 / 50% (fixed-size
clouds at 50%: 28.0), Dushanbe snow 28.3 / 17.4 / 10.9 (22.6), Arizona anvils 34.0 / 21.8 / 14.0 (28.1). Rain/snow
drops (drawn in the half-res composite) get softer and larger below 100%. The paragraph below predates this.

**Its value has shrunk since the pipeline unification.** `scene_depth.comp` and the cloud targets
are fixed at half the SWAP extent and do not scale, so at 1920x1009 dropping to 50% removes only
~1.46 Mpx of sky-pass work while ~0.96 Mpx of compute stays. With the sky pass now much cheaper
(beam occlusion gone, layers clamped at march time), 100% and 50% measure comparably in practice —
and 100% additionally gets exact hardware-depth occlusion for satellites/stars. Prefer 100%. If
render scale needs to matter again, the fix is making those two compute passes scale with it.

---

## Subsystem: Weak-Hardware Sky Tiers (Potato / SKY_LITE)

`sat_sky.frag` is ~2900 lines. On a **2015 MacBook Pro (AMD Radeon R9 M370X / GCN 1.0, macOS 12,
MoltenVK)** it compiles to one Metal fragment function whose register pressure collapses wavefront
occupancy — measured **~490 ms/frame, the entire frame**. This is not tunable by any quality slider
or `debugDisableMask` bit: those skip *execution*, not compiled *size*, and the constraint is peak
VGPR count + total texture-fetch latency that must be hidden, not instruction count. See
`SKY_OPTIMIZATION_PLAN.md` for the full investigation and `.gputrace` capture workflow
(`tools/make_capture_bundle.sh` — needs full Xcode to read).

Two stand-in fragment shaders, both bound through **`skyBgPipeLayout` / `skyDescSet` unchanged**
(each declares only the bindings it reads) and selected in `recordDraw()` Pass 1 by
`debugDisableMask` bit:

| Tier | Bit | Pipeline | Shader | Notes |
|---|---|---|---|---|
| **Potato** | `262144` | `skyBgMinimalPipeline` | `shaders/sat_sky_minimal.frag` (own file, ~370 lines) | closed-form analytic atmosphere (Kasten-Young airmass, one 32-tap arithmetic loop — no raymarch), day/night + city-detail textures, one flat drifting cloud shell w/ terminator lighting, cheap ocean (1 noise-tap slope + Fresnel + Blinn glint), textured moon, verbatim `lensFlare()`. **~60 FPS.** No Milky Way / volumetric clouds / aurora / airglow / real ocean waves — those don't fit the GCN1 occupancy ceiling (~31–32 KB SPV; the Milky Way's `atan2`/`asin` + panorama fetch is the specific thing that broke it). |
| **SKY_LITE** | `524288` | `skyBgLitePipeline` | `sat_sky.frag` **recompiled with `-DSKY_LITE`** → `sat_sky_lite.frag.spv` (2nd `add_custom_command` in CMakeLists) | `#ifdef SKY_LITE` cuts inside the real shader: Milky Way block, 64-bin satellite sky-glow loop, cloud layer loop `3→0` becomes `1→0`, the 3×3 `cloudTargetA/B` rgb blur → single tap, aurora surface glow (`auroraGlowAt` on terrain+ocean), zenith-ambient `N_ZT` 4→2, and the per-atmosphere-step **green/sodium airglow** (two `warpPerlin3` masks/step — the dominant cost) — city-glow upwelling KEPT but only on the first 3 march steps. **2 FPS → ~55 FPS** on the target. |

262144 wins if both bits are set. The always-on `Log::line` breadcrumb in `recordDraw` reports
`MINIMAL` / `LITE (SKY_LITE)` / `FULL sat_sky.frag` on any change (into `satlight_log.txt`) — this
is also the instrument for `potato-mode-intermittent-slow-start` (see memory).

**Preset wiring** (`applyGraphicsPreset`, `SatelliteSimUI.cpp`): **Potato** sets `kBitMinimalSky`
plus every compute-side knockout; **Planetarium** sets `kBitLiteSky` + `kBitCloudMarch |
kBitCirrusMarch` (coverage 0 at that tier) + `viewSamplesMin/Max 4/10`. Medium and up keep the full
shader unchanged. `GraphicsPreset::Potato` is enum value 6 (appended after `Custom` to preserve
persisted int indices); `kGraphicsPresetNames` and the preset-row UI list it first.

`skyLowResPipeline` (renderScale < 1.0 prepass) is **not** given a lite/minimal variant — both
those presets force `renderScale = 1.0`, so the prepass never runs for them.

### All-platform changes that came out of this pass (land on `main`, not tier-gated)

- **Sun corona bridge** (`sat_sky.frag` "Sun disc + atmospheric corona"): three stacked
  `pow(cosA, 1800/320/55)` lobes peaking at ~disc brightness, added between the hard disc and the
  `×0.12` wide `corona` Gaussian. Without it the disc dropped straight to the dim corona at its
  edge — a hard brightness step that read post-tonemap as a dark ring / "cutout." Same shape as
  `sat_sky_minimal.frag`'s Potato corona so the tiers match.
- **Moon `squish` dead code removed**: the disabled (`squish = 0`) atmospheric-refraction block
  still ran an `asin` + (below 15° elevation) two `tan` + `radians` to feed the identity
  `dir.z * (1.0 + 0.0)`. Gone; ray/disc intersection uses `dir` directly.
- **Cloud-shadow blur 5×5 → 3×3** (`kShadowBlurSpread = 1.7` keeps the ~radius-2 footprint;
  centre tap reuses the already-sampled `cloudBCenter.a`): −17 texture samples on every ground-hit
  pixel, the largest sample-count cut in the file. If ocean graininess returns, strengthen
  `cloudGroundShadow`'s dither in `cloud_march.comp` rather than widening this back out.
- **Push constants trimmed to the 128-byte `maxPushConstantsSize` floor** (2026-09-07). The same
  old AMD integrated parts that need Potato/Planetarium also report exactly the Vulkan-guaranteed
  minimum `maxPushConstantsSize` of 128, and `SatDrawPC` (176) / `CloudMarchPC` (148) both blew
  past it — the app could not create those pipeline layouts at all. `SatDrawPC` split into a
  128-byte sky core + a new 128-byte `PointDrawPC` for the point pipelines; `CloudMarchPC` trimmed
  to 128. Every per-frame-uniform tail field (`debugDisableMask`, `screenSizePx`,
  `skyGlareVisibility`, the `beam*` scalars, `mwSuppressEased`, `showBeamDebugRays`,
  `cloudShadowRangeM`) moved into the CloudParams frame UBO — see the **SatDrawPC** / **PointDrawPC**
  entries under "Subsystem: GPU Orbital Pipeline → Push constants" and the "Push-constant relief"
  block in `GpuCloudParams`. **Consequence for future work:** `SatOrbitPC` and `SatFlarePC` are
  already at exactly 128 — any new push-constant field on any of these structs must go in a UBO,
  not the push constant. Prefer the CloudParams UBO where the consumer already binds it.

---

## Fixed Simulation State

**Start time**: UTC 2036-11-22 02:06:22 (`kInitWholeSec`; the cloud drift's reference epoch `kCloudDriftEpochS`
is still 2036-06-21 00:00:00 = J2000 s 1,150,891,200). Sim time is real UTC (the Earth rotation angle is GMST).
**Observer**: 67°S 67°W → ECEF `obsDir = {0.1527, -0.3596, -0.9205}`, facing north — this is only the
compiled-in fallback used before `loadSettings()`/the intro cinematic override it; see below for
where the observer actually starts in practice.

---

## Subsystem: Intro Cinematic (UC3)

`showIntro` drives `updateIntroCinematic()` (`SatelliteSim.cpp`, called from `recordCompute()` in
place of normal WASD/zoom input) through a fixed beat sheet, `kIntroKeyframes[]`
(`SatelliteSim.h`). `buildIntroOverlay()` (`SatelliteSimUI.cpp`) only draws the caption/skip-hint
text on top — the camera path itself *is* the cinematic. Whether it plays on launch is a real
persisted toggle, `playIntroOnStartup` (Display tab "Play intro on startup") — see the persistence
note near the bottom of this section; it is no longer forced on every launch.

**Window boots maximized** (`App::initWindow`, `glfwWindowHint(GLFW_MAXIMIZED, ...)` before
`glfwCreateWindow`) — windowed, not exclusive fullscreen, but full monitor work-area size by
default rather than the fixed `WIN_W`x`WIN_H` (1280x720). A small window the player has to
manually enlarge undercut the cinematic's impact and invited resizing instead of watching it.
`WIN_W`/`WIN_H` are still passed as the restore size for whenever the player un-maximizes later.

**Fixed vantage, locked every playback.** The intro always opens from `kIntroObserverLatDeg/LonDeg`
+ `kIntroStartAzDeg/ElDeg/FovDeg` (`SatelliteSim.h`) — the California coast at twilight facing the
SpaceX AI-datacenter satellites and the Reflect Orbital mirrors aimed at the nearby Topaz solar
farm — not whatever the player's current/saved observer position happens to be.
`updateIntroCinematic`'s one-time init block (`!introBasisValid`) force-sets
`obsDir`/`obsLatDeg`/`obsLonDeg`/`obsHeightOffset`/`camera.azDeg/elDeg/fovYDeg` from these
constants before computing the East/North tangent basis the rest of playback rides on. The
Display tab's "Replay Intro" button resets `introBasisValid = false`, so a replay re-locks to the
same spot regardless of where the player has since wandered off to. Also forces
`timeScaleIdx = 0` / `timePaused = false` / `timeDir = 1.0f` here — the beat sheet is tuned at 1x
and a replay runs on live state, not a fresh boot, so it can't rely on `loadSettings()` alone.

**`camera.azDeg` vs `obsFacing` — read this before touching keyframe azimuth.** `camera.azDeg` is
what `SkyCamera::viewMatrix()` actually renders with; `obsFacing` is only the ground-movement
tangent, and outside the intro `camera.azDeg` is *derived from* `obsFacing` every frame in
`buildUI` (search "Derive camera.azDeg from obsFacing"). That derivation is skipped entirely while
`showIntro` is true (same gate as the RMB-look block), so `updateIntroCinematic` must set BOTH
every frame — it used to set only `obsFacing`, which meant the rendered view never actually panned
in azimuth during playback and then snapped to match `obsFacing` on the first post-intro frame.
Fixed by computing the mixed azimuth once and assigning it to both.

**Beat sheet** (`kIntroKeyframes[]`, timings synced to `songbeat` = 7.61s, a music-tempo unit —
hand-tuned by the user, not derived): `kIntroYearIndex` (0) is a "2036" title card, bottom-anchored
like every other caption but at a larger font size (not centered — that was tried and reverted, it
read as inconsistent with the rest of the captions); `kIntroHintRevealIndex` (1) is the first
narrative line and also where the bottom-right skip hint first appears (not from frame 0 — see
below); the middle beats hold at ground level then pull to LEO; `kIntroTitleIndex` (6) is the
arrival "SAT LIGHT SIM" reveal; beat 7 (the last key, text null) settles the final framing while the
title fades, and the intro ends there (`kIntroBenchEndT` = its time: the whole intro feeds the UC1
benchmark).

**Camera path is a Catmull-Rom/cubic-Hermite spline, not per-segment smoothstep.** The original
implementation eased in/out (smoothstep) independently within each keyframe segment, which gives
zero velocity at *every* waypoint, not just the first and last — the camera visibly decelerated to
a stop and re-accelerated at every beat boundary, reading as a stutter rather than one continuous
move. `updateIntroCinematic`'s local `hermite(field)` lambda instead estimates a time-weighted
tangent at each interior key from its two neighbors and blends with cubic Hermite basis functions,
so velocity carries through a waypoint instead of resetting there. The start's tangent is the
one-sided difference (~0: beats 0-1 share their values); the LAST key's is zero, so the camera eases
to a stop on the handoff frame.

**No controls in the intro (2026-10-03).** Beat 7 used to be a "WASD to move / Q-E / click a
satellite" caption that unlocked movement and look (`kIntroControlsIndex`) while clicking stayed
disabled until the intro ended — and the post-intro "Click to select any satellite" hint
(`buildSelectHint`) then said it again. Both are gone: input is live only once `showIntro` is
false, and `finishIntro` starts the first-run tutorial (below).

**Dismissal is a single defined key, not "any key."** `onKey()` only calls `finishIntro(true)` for
literal `GLFW_KEY_SPACE` (independent of whatever `Pause/Resume` is currently rebound to);
`pollGamepad()` only responds to `GLFW_GAMEPAD_BUTTON_START`. Mouse clicks do NOT skip —
`buildIntroOverlay` still covers the screen with a mouse-capture rect so a click doesn't leak
through to satellite picking, but it no longer calls `finishIntro`. This was a real usability
issue in the previous "any key/click" version: almost no one saw the cinematic play out, because
touching the keyboard or clicking into the window to focus it immediately skipped it.

**The intro hides the entire normal HUD**, not just its own overlay — `buildUI()` checks
`showIntro` before icon loading/scroll-zoom/satellite-picking and before the `uiVisible` check, so
none of the left/right HUD panels, settings/view-controls windows, or scene-interaction input run
while the cinematic owns the camera, regardless of whether the player has the rest of the UI
toggled on or off.

**`timeScaleIdx`'s compiled-in default was `1` ("10x"), not `0` ("1x")** — a real bug, now fixed.
`loadSettings()` overrides it from `settings.json`'s `time.scale_idx` when present, which is why it
looked "sometimes" wrong: a genuine first run (or any state where that key was never written)
booted at 10x. The intro's one-time init above also force-sets it defensively, since a replay runs
on live state that loadSettings() never touches again.

**Persistence: `playIntroOnStartup`, not a raw `showIntro` save.** `showIntro` is runtime
play-state (flips false the moment the cinematic ends), so persisting it directly conflated "did
today's playthrough finish" with "should it play again next launch." `playIntroOnStartup` (Display
tab checkbox, next to "Replay Intro") is the actual user preference, saved every close and applied
to `showIntro` at load time: `playIntroOnStartup = d.value("play_intro_on_startup", false)` inside
the `"display"` block. Absent-key default is `false` there (not the compiled-in `true`) so a
player upgrading from a build that predates this key doesn't suddenly get a cinematic that didn't
exist in their version — a genuine first run (no settings.json at all) never reaches that line, so
it still keeps the compiled-in `true` default from the "no file" early-return branch. Disabling it
does not need to separately handle "resume at last known location" — `obsDir`/`camera` are always
restored from settings.json's own `"observer"`/`"camera"` blocks regardless of `showIntro`, so
turning the intro off just resumes wherever the player last was.

---

## Subsystem: First-run tutorial (2026-10-03, `SatelliteSimTutorial.cpp`)

One card at a time (`buildTutorial`, z 40) — a low, wide strip, the graphic beside the text, in the HUD's
palette (`Pal` / `Style`, moved to `UIPalette.h` so every UI file shares it; wanted input pulses in the
red accent, held input is solid red) — nine steps (`TutStep`; a tenth, Graphics, only on the light presets: "no
clouds", the preset to try, done by opening Settings or changing the preset; `tutStepCount()`): look, move, climb, boost, select —
each finished by DOING it, with a keyboard (Q W E / A S D / Shift, labels from the live bindings), mouse
or gamepad graphic (`buildTutKeyboard/Mouse/Gamepad`; the pad's when `lastInputWasGamepad`), plus a progress bar — then the selection's buttons, the
time controls, the picture buttons and the menu buttons (Bookmarks, Cinematics, Settings), each OUTLINED on the HUD (PASSTHROUGH floating boxes
from last frame's layout) and finished by using one of them (a change from the state the step began in,
`tutBase*`) or Next. A finished step shows "Done!" for 1.1 s and moves on. The card sits low in the
middle, above both corner panels, or above the panel it points at (`CLAY_ATTACH_TO_ELEMENT_WITH_ID`).
- **Starts** from `finishIntro` (not on a replay of the intro), or on the first frame in the scene when
  the intro is off (`updateTutorial`, `tutAutoChecked`), once: Skip and Finish both set
  `display.tutorial_done` (absent = not done, so existing installs see it once). Settings > Display >
  STARTUP > Replay tutorial runs it again. A harness run never starts it on its own.
- **Detection** (`updateTutorial`, in buildUI right after the look block and before the click pick) reads
  the keys and gamepad values itself; it does not hook the movement code.
- **Clay keeps stale boxes** for elements not drawn this frame, so the selection buttons' outline uses only
  the `SelActBtn` ids `buildSelActionButton` drew this frame (`selBtnDrawnMask`).
- `keyDisplayName` / `gamepadButtonDisplayName` (SatelliteSimUI.cpp) are no longer file-static; keyDisplayName's
  letters rotate through a 4-slot pool, so keycap labels are copied (`tutKeyLabel`).
- A controller moves through the card with **Start = Next / Finish, View = Skip** (`padContextButton`).
- Harness: `tutorial start [step] [pad=1] | step <n> | next | back | skip | state`;
  `tools/harness/scripts/tutorial.satcmd` captures every card and clicks through the HUD steps.

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
- `SatDrawPC`, `PointDrawPC` and `CloudMarchPC` are all exactly **128 bytes** — the
  `maxPushConstantsSize` floor. `debugDisableMask` and the other per-frame tail fields ride in the
  CloudParams UBO now (`cloud.dbgDisableMask` etc.); see the **SatDrawPC** / **PointDrawPC** entries
  under "Subsystem: GPU Orbital Pipeline → Push constants" and the "Push-constant relief" block in
  `GpuCloudParams`. Both point pipeline layouts (`drawPipeLayout`, `starPipeLayout`) use
  `sizeof(PointDrawPC)`; `skyBgPipeLayout` uses `sizeof(SatDrawPC)`.
- Sky descriptor set has 29 bindings (0-28; 28 = the major-roads storage buffer for the city lights; 27 = the terrain material array, terrain v2 P3; 26 = terrainFrameBuf, the observer's detailed ground height
  from scene_depth.comp, 2026-09-25). Before that it had 26 (0-25; 22/23 the mesh targets, 24 the env star grid, 25 the
  sharp-reflection G-buffer — the last two read only by the SKY_ENV / SKY_REFL variants). The original 22 (0-21): GlowBuf, noise, moon, earthDay, earthNight, earthElev, earthSpec (the R8G8 water map since terrain v2 P2), earthClouds (since 2026-09-29 the v2 weather CUBE), cloudNoiseTex (sampler3D), CloudParams UBO, half-res cloud march targets A/B, lightDomeBuf, milkyWayTex, cityDayDetail, cityNightDetail, auroraNoiseTex (sampler3D), reflectBeamsBuf, beamGlowDomeBuf, sceneDepthTex, oceanGlintBuf, groundBeamsBuf. Binding 18 was `cloudShadowTex` until that pass was deleted; 19/20 were compacted down into 18/19 rather than leaving a hole, since the C++ side fills its binding array contiguously. groundBeamsBuf (21, perf follow-up) is the CPU-compacted, observer-range-culled subset of reflectBeamsBuf that sat_sky.frag's ground-spot loop reads instead of the raw (up to 2048-entry) buffer — see GpuGroundBeams in SatelliteSim.h. **As of 2026-08-10 its entries are `GpuGroundBeam` (32 bytes), not raw `GpuReflectBeam`** — a pre-solved record, see "Beam ground-spot CPU hoist" below
- GPU-side observer ground height lookup added; CPU observer height also corrected (see elevation encoding below)
- `sat_sky.frag` ground path: terrain march step count is path-length-adaptive as of session 29
  (`kN` scales with this ray's own `tExit`, clamped to a user-tuned [64,164] range — the old
  altitude-only formula gave a grazing/horizon ray and a steep ray from the same observer altitude
  identical step budgets regardless of how much further the grazing ray actually travels, a real
  bug contributing to reported terrain jitter, not just under-tuned; see `TERRAIN_PLAN.md` session
  29 log) + 12-step binary search; terrain hits use gradient-computed normals; sea-level sphere
  fallback; satellites/stars depth-tested against terrain (gl_FragDepth — since Phase 4 the unified log-distance
  encoding, see "Unified scene depth"; originally close terrain → [0, 0.5),
  sky → 1.0)
- **The atmosphere loop starts where the ray ENTERS the atmosphere** (`tStart`, 2026-09-29), not at the eye:
  from orbit nearly every N_VIEW step fell in the vacuum above R_ATMOS (zero density, but each still paid the
  city-glow fetch, the trig and the airglow noise) while the few left in the air undersampled it. Knockout
  sweep at 2000 km: the loop was 13 of the sky pass's 16.6 ms. Sky pass 420 km 6.9 -> 3.3 ms, 2000 km
  16.5 -> 4.9, 10000 km 5.2 -> 2.3; images within ~1/255 (harness_runs/impostor). Every other atmosphere
  integral (cloud airlight, airglow red, godrays) already started at the entry. The same measurement showed
  a far-field cloud impostor is not worth building: the cloud march gets CHEAPER with altitude (10 ms at
  420 km, 8.5 at 2000, 4 at 10000, 2 at GEO), and at LEO a pixel is only ~1-2 km, finer than a globe-wide bake.
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
- **Aurora sheets (2026-09-30, `cloud_march.comp` auroraSheetIndex / auroraSheetLight, in
  auroraMarchSegment).** Thin emissive curtains over the diffuse volume: the level sets S = n of an index
  field (colatitude / spacing + a sum of travelling waves; where its gradient across the oval passes 1 the
  sheets fold into curls). Each ray crossing is integrated EXACTLY: emission x sigma sqrt(2 pi) / |dS/dt|
  (the path length through a Gaussian sheet, capped at two steps), so edge-on curtains are crisp ribbons at
  any resolution with no sampling noise; crossings are found between the volume march's own samples and
  refined by two regula-falsi steps on the exact index. **Review 11:** each sheet is integrated over each STEP
  with S linear in t (erf of both ends; candidates within 1.2 sigma of the step's S range, faded at the edge), and
  the march start is jittered per pixel and per frame (IGN + the TAA's Halton phase, `gAurJitter`). The first cut
  credited a whole Gaussian (capped at two steps) to the step holding a crossing and nothing otherwise: where a ray
  grazed a fold, detection flipped with the elevation, drawing stacked horizontal zigzags (user report). Without
  the jitter soft bands remain (piecewise-linear S). Cost +0.55 ms in a Coldfoot storm view (the first cut +0.21;
  3-sigma candidates +0.9). Crisp or fuzzy per sheet and per ~500 km of oval;
  a sharp lower border (pink N2 edge), rays (field-aligned striations: from the ground they converge on the
  magnetic zenith as a corona), red tops. The index is carried across EVERY sample (dropping it where the
  oval mask was 0 cut the sheets in whole-step chunks: a staircase). Folds finer than the 15-km steps alias
  (a crossing and its return inside one step are missed): the 71x wave does not scale with "Sheet folds",
  capped at 2. Cost +0.55 ms of cloud march in a storm. Settings (Atmosphere tab, aurora): "Curtain sheets"
  `clouds.aurora_sheets` 1, "Sheet spacing (deg)" 0.3, "Crisp sheets (share)" 0.5, "Sheet folds" 1 (slots
  215-218, UBO `auroraSheets`). The aurora's light on the ground (terrain and sea) was REMOVED the same day
  (the user: green and bad); `auroraContribTerrain` stays as a zero term. Test from a DARK site (Coldfoot,
  67.25 N 150.18 W, December): Fairbanks' skyglow gates the aurora out. `scripts/aurora_sheets.satcmd`.
- **The auroral oval + space weather (2026-10-03, `include/aurora_oval.glsl`, `src/simulations/SpaceWeather.h/.cpp`).**
  The oval is a RING fixed relative to the SUN: magnetic local time from `cloud.auroraMidnight` (the anti-Sun
  direction projected on the dipole's equatorial plane), widest and furthest equatorward at ~23 MLT, thin at noon,
  the polar cap inside it dark; a sharp poleward edge (discrete arcs) and a soft equatorward one (diffuse aurora;
  the sheets carry a quarter of their light there, `auroraDiscreteShare`). Its edges are Feldstein-Starkov-like
  functions of an activity index Kp (`auroraOvalBounds`: midnight equatorward edge 66.5 - 2.1 Kp MLAT, Kp 9 ->
  ~47), brightness 0.4 + 0.2 Kp (1 at Kp 3). It replaced a band at 20 deg colatitude (+8 x storm) that faded
  out over TWICE its width: at the default storm it was lit from ~5 deg off the pole, a cap. **Kp is a pure
  function of sim time** (`spaceWeatherAt`, CPU double, each frame into the UBO's auroraMidnight / auroraOval /
  auroraSub / auroraOval2, 864 -> 928): the 11-year cycle (cycle 25 min Dec 2019), recurrent coronal-hole
  streams (27.27 d, a hole lives ~6 rotations), CME storms (a daily Poisson chance x cycle x the Russell-McPherron
  equinox bias; sudden commencement, a 3-9 h main phase, a 0.5-1.3 d recovery; peak Kp from knots fitted to
  NOAA's per-cycle counts — measured 936 / 501 / 174 / 103 / 9 three-hour intervals at G1-G5 against NOAA's
  1700 / 600 / 200 / 100 / 4), and substorms (2.5-h bins, ~2.5 a day, more with activity: onset near 23 MLT,
  a bulge pushed 2-6.5 deg poleward that widens and drifts west, brighter and filled in, ~2 h). Time reversal,
  warp and bookmarks see the same storm. `cloud.stormStrength` (fold chaos, coverage fill) is now DERIVED:
  (Kp - 1) / 9, max 0.85 (at 1 the coverage gate filled the oval). Atmosphere tab: "AURORA NOW" readout,
  "Space weather (0 manual / 1 auto)" (slot 258), "Aurora Kp (manual)" (slot 25, was "Storm strength"),
  "Storm frequency (x)" (slot 259); keys `clouds.aurora_activity_auto / aurora_kp_manual / aurora_storm_rate`
  (`storm_strength` is no longer read). The ambience's `aurora` driver uses the CPU mirror
  (`auroraOvalWeightCpu`). Harness `aurora` (docs/HARNESS.md). Open: a storm from orbit is a filled ring of
  concentric bands (the coverage noise varies with colatitude by design); the noise azimuth is still Earth-fixed
  while the oval is Sun-fixed; no dawn-side omega bands / pulsating aurora; the dipole axis is fixed (no IGRF).
- **Next:** C14 (Anvil) remains not started — deferred repeatedly in favor of C15/C16 per the
  2026-07-12 session — and can be picked up whenever; it has no dependency on C15/C16. Otherwise
  Phase E is complete (C13, C15, C16 done); C9/C11/C12 and noise-repetition cleanup are next in
  line per the 2026-07-12 planning session's priority order. Resolution scaling shipped in session
  29 (background-only, satellites/stars/UI always native res) — see "Subsystem: Resolution
  Scaling" below.

### Procedural terrain detail (2026-09-25) — `shaders/include/terrain_detail.glsl`

Built overnight with the automation harness (docs/HARNESS.md) as its first real client; the
`erosion` branch's approach (a one-Newton-step displacement at the smooth hit + a normal perturbation)
was studied and not merged — its own notes say a real 3D surface was the goal and could not be
reached by patching the bisection. This is that surface. Read the header comment of
terrain_detail.glsl first; invariants and the reasons behind them:

- **The surface is `H = DEM + D`, and every pass that needs "where is the ground" uses the same
  function and the same march** (`terrainMarchDetailed`): sat_sky.frag, scene_depth.comp,
  terrain_probe.comp. D is eight octaves of 3D value noise (2048 m .. 16 m cells) on the sea-level
  sphere, slope-damped (IQ-style erosion look), amplitude = `terrainDetailAmpM` x roughness (the DEM's
  SIGNED relief against mip 3 — ridges rugged, valley floors smooth; |relief| dug pits into every
  valley floor) x coast fade. The geometry LOD (`tdGeomLodM` = max(2 px, 1.2% of t)) depends on t
  alone at any normal FOV, which is what makes the half-res and full-res passes see the SAME surface.
- **Never form an absolute ECEF coordinate in float.** The lattice is anchored to the observer's
  sea-level point as an integer 2048-m cell + offset (CPU double -> `terrainAnchorCell/Rel`); every
  height uses the observer-relative `q` (`tdAltitude`, `tdSphereOffset`). The erosion branch fought
  exactly this precision loss (stretched/swimming noise).
- **A seed pyramid: quarter-res -> half-res -> full-res.** scene_depth.comp runs twice: a
  quarter-res pre-pass (`quarterPass`, its own descriptor set `sceneDepthQDescSet`, writes
  `sceneDepthQImg` and `terrainFrameBuf`) marches the true surface from the eye; the half-res pass
  starts from the nearest of its 2x2 texels (x0.995) or skips rays where all four were sky; sat_sky.frag
  does the same from the half-res result and usually resolves in 1-5 steps + a 3-step regula falsi.
  Conservative at every level because each coarser pass's pixel-footprint test is wider, so it stops
  EARLY and counts near misses of a crest as hits. Each set's binding 7 samples the OTHER image (so
  neither is read in the layout it is being written in). The first cuts (full-res march from the eye;
  an envelope depth) cost 10-25 ms at ground level; measured with `perf` + `debugview steps`.
- **A step past the exit lands ON it** (`atExit`, 2026-09-29): the step floor (1.5-5% of t) is ~1 km at
  70 km, and over low land a steep ray's last step jumped from above the ground to past the sea sphere:
  a MISS, drawn as flat sea-level land in rings about the nadir (user snapshots 2, 8 — found with
  `probe`: "from eye -1 m (34 steps)" alternating with hits). The loop now clamps once to tExit, so the
  crossing is bracketed. Shared with the depth passes.
- **Past the march's range, land sits at its DEM height** (sat_sky.frag, two re-intersections of the
  sphere R + h): on the sea sphere its air column was the whole atmosphere to 0 m — from orbit the
  Tibetan plateau was clear inside the range and white-veiled past it (user snapshot 1's disc).
- **Out of budget is not a miss**: the march finishes on a coarse-octave tail. Returning -1 made the
  depth say "sky" and the sky pass skip the pixel — holes through distant hills (found with `probe`).
- **The observer's ground includes the detail** (`observerEffHeightDetailed`), computed once by
  scene_depth.comp into `terrainFrameBuf` (sky binding 26) and per texel by cloud_march.comp.
  beam_self_march.comp still takes the CPU's obsEffH (pre-existing), and the mesh shaders use the
  DEM-only `observerEffHeight` — both off by at most the detail height.
- **Shading**: detail normals at a 3 px LOD (finer aliases into paper-like grain), plus shading-only
  micro relief (`tdMicroBump`, 8 m .. 0.25 m, gradient noise, SOLID 3D so steep faces are not
  vertical smears) and a solid albedo mottle (`tdMicro`). Materials key off the day map (its white =
  snow, rock on steep faces, darker where the map is snow); a latitude snowline painted Tibet white.
  All fade out by a ~400 m pixel footprint, so orbit views are the day map untouched. Soft terrain
  sun shadows (16 steps, 3 octaves, normal-offset start — starting on the surface gave texel-sized
  acne, and lifted onto the coarse surface the ray is tested against) with a 15% bounce floor.
- **Lighting of faces turned from the Sun** (found flying the harness into a glacier, where half
  the metre-scale facets of a snowfield went pure black under a 20-degree Sun):
  - `dayFrac` is the GEOGRAPHIC horizon gate only. It was also gated by the shading normal, which
    sent every face turned > ~8 degrees past edge-on to the Sun to the NIGHT branch: no skylight,
    and the city-lights map, in daylight. DEM normals seldom got there; detail normals constantly do.
  - The direct term's 0.05 floor is reached smoothly (`sunLit`); its hard clamp drew contour rims.
  - A ground bounce proportional to the day map's luminance and the Sun's local height
    (`bounceK`, scaled by the material strength): snow lifts its shaded faces, forest barely moves.
  - `tdMicroBump` is 0.3x on day-map snow, and its floor on smooth ground is 0.25 (was 0.35).
- **Erosion octaves (`tdErosion`, 2026-09-25)** — the `erosion` branch's algorithm (clayjohn 2018 /
  Fewes 2023 "eroded terrain noise": per-cell cosine stripes whose phase runs across the slope, so
  gullies run downhill, each octave following the slope plus the octaves before it = branching),
  made part of the marched surface: two octaves (512, 256 m) between the coarse and fine value
  octaves, amplitude `terrainErosion.x` x octave-0 amplitude x a slope factor, bounded, so the march
  stays conservative (`tdErosionBound`, `tdRemainingBound`). Sliders "Erosion strength" (0.6) /
  "Erosion branching" (1.0), keys `clouds.terrain_erosion_*`; `debugview erosion`.
  - **Domain**: the ECEF plane perpendicular to the dominant axis of up (drop one coordinate — the
    anchor's integer cell carries over exactly, so nothing swims), blended across cube-face edges.
    Stripes vary across the PROJECTED slope, so grooves run exactly downhill whatever the projection
    stretch. The Alps sit on the x/z face edge — views there pay for two faces in a narrow band.
  - **Kernel**: compact (1 - d^2/R^2)^3, R = 1.25 cells, points jittered within the middle half of
    their cell, so the 3x3 neighbourhood is exact (no cell-border seams); per-point frequency
    (+-25%) — one frequency read as regular ripples on a far slope; the erosion needs a cell of 4x the
    LOD for full weight (twice the value octaves' margin) for the same reason.
  - **Steering**: the DEM slope by central differences over +-1 texel (continuous across texel
    borders) plus the coarse octaves' gradient. Where that direction flips (crests, valley floors)
    the stripes crowd into fringes — visible in `debugview erosion`, faint or hidden by haze in the
    render; steering by the smoothest slope only removed them but also most of the effect (tried).
  - **Cost is what shaped the code.** GLSL inlines everything and the registers are sized for the
    largest path, so erosion code that never runs still slowed every terrain pixel: a first cut with
    the erosion inside the octave loop (copied into every unrolled iteration), two face-evaluation
    sites and three full-height sites in the march made the ground views ~50% slower with the
    erosion switched OFF. Now: one erosion call site outside the value-octave loop (`tdOctaves` =
    value octaves, erosion, value octaves; `allowEro` literal false on coarse-only paths, which use
    `terrainHeightCoarse`), one face site, `[[dont_unroll]]` loops (GL_EXT_control_flow_attributes in
    every shader including terrain_detail.glsl). The march's refinement and the shading normal take
    the erosion as the plane the last full evaluation left (`terrainHeightLinEro`,
    `terrainDetailLinEro`, `gTdHitEro`) — the gullies are flat to centimetres over a bracket. A
    per-cell cull (skip cells out of the kernel's reach) was SLOWER (divergence), a third 128-m
    octave cost ~1 ms for little. The terrain shadow's lift reuses the normal's coarse height (`hC3`).
  - **Measured** (RTX 3070 Ti, 1600x900, High, harness perf): +2.5-3 ms on the ground in mountains
    with clouds off (v1 13.0, v4 13.7 ms), ~0 from 10 km up; Grand Canyon rim with clouds at High
    15.8 ms (p90 16.4) vs 13.0 without, at Medium 12.8 vs 10.7.
- **"Erosion size" (review 6, `clouds.terrain_erosion_size`, slot 211):** the erosion octaves' cells
  x 0.5 / 1 / 2 / 4 (256-m .. 2048-m first cell), powers of two only so the lattice stays exact on the
  2048-m anchor cells (`tdErosionK`, `tdEroAnchor`: cells past 2048 m take the anchor's remainder into
  the lattice coordinate). Carried in `envMainObsDir.w` (log2 of the factor; the w was unused). On
  Fuji (a smooth cone in the DEM) the lumps are mostly the erosion, the rest the value octaves: erosion
  0 leaves soft bumps, "Detail height" 0 the clean cone.
- **Empty-space skipping was tried and removed**: a CPU-built max-mip chain of the DEM, tested in
  the depth pass where the ray cleared the local max + the detail bound. It made the depth pass
  SLOWER at every altitude (v4 3.3 -> 4.5 ms): the rays that cost are the ones just above the
  horizon, and those are within the bound of the local max for tens of km, so the extra fetches on
  every step bought almost no skipped steps. (Its upload also taught that Vulkan mip sizes round
  DOWN — the validation layer via VK_INSTANCE_LAYERS caught it; see docs/HARNESS.md.)
- **The DEM is sampled exactly near the observer** (`tdDemTexel`/`tdDemBilinear`, within 4 km):
  the float UV resolves ~2.4 m and hardware bilinear of R8 uses 8-bit sub-texel weights; together
  they built metre-high shelves on a steep wall seen from 10 m. The CPU passes the observer's texel
  (integer + fraction, `terrainObsTexel`); each point adds a cancellation-free lon/lat offset.
- **Cost (RTX 3070 Ti, 1600x900, clouds off, harness `perf`, final)**: +4-6 ms at High on the ground
  in mountains (worst total 9.7 ms vs ~4 ms without detail: depth passes 1.5-2.1 ms, sky 4-5 ms incl.
  1-2 ms shadows), ~+1 ms from aircraft, ~0 from orbit; +3 ms at Medium in the Anchorage worst
  case (measured before the quarter pass; less now). Presets: on for
  Medium/High/Ultra, off for Low/Planetarium/Potato (`applyGraphicsPreset`). Terrain tab sliders:
  Terrain detail / Detail height / roughness / erosion (the value octaves' slope damping) / Terrain
  shadows / Terrain materials / Erosion strength / Erosion branching (the gully octaves).
- **Tools**: `tools/harness/scripts/terrain_views.satcmd` (eight golden views), harness `debugview`
  (normals, detail, steps, albedo, shadow, rough, elevzebra, distzebra, erosion) and `probe x y`.
- **Known limits**: the ground within ~20 m is soft (it would need real texture maps); silhouettes
  are not antialiased; value noise shapes; the DEM itself is 2.67 km / 8-bit (the Everest region is a
  smooth plateau in it); SKY_LITE and SKY_ENV draw the plain DEM (`tdEnabled()`).
- **Night and ambient light (terrain v2 P1, 2026-09-29, `.plans/TERRAIN_V2_PLAN.md`):** the night map
  is a Black Marble composite with a BLUE base under every texel (land sRGB ~(12,13,25)); used as the
  terrain's emission it out-shone full-moon ground and made the night side a flat blue 5 km texture.
  The terrain now takes only the LIGHTS from it (`cityLights` = night map - linear (0.006, 0.006,
  0.0132), a plain subtraction — a knee on the filtered value drew every texel as a hard square from
  orbit), and the night surface is the day albedo lit by the Moon, the moonlit sky (0.15 of the direct
  Moon) and the moonless night sky ("Night sky light", `clouds.terrain_night_sky_light`, 0.25 of the
  full Moon overhead: the relief reads, dark). The sky ambient takes a sky-view factor (0.5 + 0.5 n.up)
  and a gain ("Terrain sky light", `clouds.terrain_sky_light`, 1). Both ride in `terrainErosion.zw`.
  Only the terrain emission changed: the dome, ambience and cloud city glow read the night map with
  their own response curves; Potato (`sat_sky_minimal.frag`) still draws the raw night map. Harness:
  `debugview nightsky` / `citylights`, `scripts/terrain_night.satcmd`. **`clouds.coverage` is the
  retired v1's — clear the volumetric clouds with `clouds_v2.coverage 0`** (a night LA view "without
  lights" was an overcast).
- **Water: lakes at their own level, shores from a distance field (terrain v2 P2, 2026-09-29).**
  Binding 6 ("earthSpecTex" everywhere) is now the WATER MAP, R8G8, baked by
  `tools/make_water_map.py` (scipy) into `assets/textures/earth_water_sdf.r8` + `earth_water_level.r8`
  and interleaved at load: R = a smoothed signed distance to the shore (0.5 + d / 2 x 19.5 km, d > 0
  in water — `r > 0.5` is still "water" for every old consumer), G = the level of the NEAREST water
  body in DEM units (each connected body's median DEM over its interior; the DEM is flat over a lake).
  The shared height function (terrain.glsl `waterAdjustHeight`, called from `tdDemAt`): a water texel
  is its body's flat level, marked `hMip3 = kTdWaterMark` (tdAmp0 0 there, and a hit tests it); land
  within 3 km is banked up to at least the level and within 1.5-3 km capped by a 0.25 m/m ramp from
  the shore (the DEM's 35-m land baseline was a step at every coast; the cap must let go, or it
  flattens mountains rising from lakes); `tdShoreOffset` moves the shoreline by one octave of
  anchored 1024-m value noise (+-700 m: coves, headlands, sea stacks at Big Sur). `sat_sky.frag`: a
  hit on a lake takes the ocean path at that level (`tSeaLvl` = the hit, `waterLevelM`), and a ray
  that fell through to the sea sphere re-tests water at that point with `tdDemAt` (`waterPx`) —
  most sea pixels come that way, and the raw map lacks the coves. Before this, water was drawn only
  where the DEM was < 160 m, at sea level: Superior, Victoria, Titicaca, Baikal were blue LAND.
  **Cost**: the map is fetched only where its mip 2 is non-zero (within ~15 km of water); in-app A/B
  (knockout bit 4194304 `water_map`, `scripts/terrain_water_perf.satcmd`): Geneva +0.5 ms, Big Sur
  from 1.5 km +0.7 ms, inland 0. The first cut fetched it on every height evaluation (+1.7 ms in the
  Alps) and a second noise octave cost +0.3-0.4 ms. The source mask's last row was all water (a
  3600-m "lake" round the South Pole) — the bake copies the row above. Re-run the bake after editing
  the mask or the DEM. Harness scene: `scripts/terrain_water.satcmd`.
- **The biplanar weights have a floor** (`tmSample`, +0.02): near the cube diagonal (|n| components all
  ~0.58, central Japan) both were ~0 and the clamped normalisation returned black material blobs and
  flat-lit relief (user snapshot 7).
- **Close-up material textures (terrain v2 P3, 2026-09-29).** Sky binding 27 (`terrainMatTex`,
  `sampler2DArray`, the stage's 16th and last sampled image): six CC0 ambientCG sets (grass, forest
  floor, rock, snow, sand, dirt) baked by `tools/make_terrain_materials.py` into
  `assets/textures/terrain_materials.rgba8` (raw, "SLTA"; mips box-filtered at load by
  `createTerrainMaterials`, a 1x1 neutral array without the file). Layer 2m = the albedo as a linear
  RATIO to the set's own mean x 0.4 + height; 2m+1 = normal XY + roughness + AO. In the hit block
  (after tdMicroBump): the day map picks the material (green = grass, dark green = forest floor,
  bright unvegetated = sand, else dirt; steep = rock; its white = snow), the two strongest are sampled
  biplanar in ECEF on the anchored lattice at 4-m tiles (2048 is a multiple, so world-fixed) with an
  explicit LOD from the footprint (no derivatives in the divergent branch), blended by height, and a
  16-m read of the main one multiplied in against tiling; the ratio multiplies the day colour (so the
  hue at distance is unchanged), the normal perturbs the shading normal, AO joins terrainAO. Fades in
  below 0.6-3 m per pixel. "Close-up textures" (`clouds.terrain_texture_strength`, the old
  `terrainPad0`, slot 57; on for Medium and up). Cost +0.27 ms on an Alpine meadow at 2 m (in-app
  A/B). Harness: `scripts/terrain_materials.satcmd`.
- **Procedural city lights (2026-09-29, `.plans/CITIES_PLAN.md` phase 1).** `cityLightPattern()` in
  sat_sky.frag MULTIPLIES the night map's lights (after the blue base is removed) on land at night.
  Layout: jittered-Voronoi districts of 4096 m (2 anchor cells, integer-anchored, on the erosion's face
  projection; an arterial runs along the face seam), but ONE street grid per region of 4x4 districts
  (angle, base spacing 90-120 m, phase and arterial interval measured from the region's origin), so
  streets run straight through district borders — districts differ only in which streets exist
  (suburbs every other street and more drop-outs); a fifth of districts lay out their own grid
  (T-junctions, organic warps). Light is LAMP POSTS, not glowing streets (user, 2026-09-29): posts
  every 35 m (30 on arterials, 3.5x brighter), each a pool on the road (sigma 6 m) + a small head
  (1.2 m, 30%), per-post brightness 0.5-1.5x and one in ten dark; they stay separate dots until the
  pool is wider than ~0.4 of the spacing, then merge into lines, then a uniform glow (normalised to the
  expected lamp density, mean 1). Macro: commercial strips along arterials, soft dark voids, a 2-km
  value noise; sodium / LED tint over ~8 km. The REAL major roads (`cityRoadLight`: Natural Earth 10m,
  647k segments baked by `tools/make_city_roads.py` into `assets/textures/city_roads.bin`, sky binding
  28 storage buffer, a 0.25-degree lat/lon cell list; distances in the anchor cell's frame, where a
  float ECEF endpoint minus the 2048-m-multiple cell origin is exact) are dotted the same way (posts
  every 40 m). The mean ramps from 0.12 of the map close up (the old tiled detail texture averaged ~0.1
  of it: a city core at the night exposure is otherwise a lit sheet) to 1 at a 350-m footprint, and
  the pattern stops at 400 m, so ORBIT IS BIT-IDENTICAL (checked: Europe from 400 km). Also on land at
  sea level (`waterPx == 0`, most of the LA basin). Knobs: "City street lights"
  (`clouds.city_lights_strength`, slot 58; 0 = the old detail texture) and "Major road lights"
  (`clouds.city_roads_strength`, slot 77), in the UBO where `oceanGlintPad0/1` were (renamed in place).
  Cost +0.8 ms over LA from 10 km (in-app A/B). Harness: `scripts/city_lights.satcmd`.
  **Not gated on terrain detail (2026-10-04):** Low and Planetarium (SKY_LITE) draw the full pattern too (it was
  `tdEnabled()`-gated, so those tiers showed the night map's blobs): +0.7 ms on Low, +2.4 ms on Planetarium over LA
  from 1.5 km (RTX 3070 Ti). Farmland and beaches still follow the terrain detail.
  **Day side (phase 2, 2026-09-29):** the same layout (`cityLayout()`, a `CityLayout` struct computed
  ONCE per pixel before the material block and reused by the night) drives `cityDayAlbedo()`: asphalt
  streets (12 m, arterials 24 m, exact box-filter coverage `cityBoxCover`), each base block split into
  2-4 lots per side, roofs with their own margins and offsets from a palette (muted terracotta in
  "warm" regions), commercial blocks (1-2 big flat-roofed buildings) along the arterials — where the
  night's bright strips are — houses between, yards lawn / paved / bare, a tree canopy (16 m / 8 m value
  noise, ~70% of suburban yards, ~30% in cores), parks in the voids. It returns a RATIO to the
  pattern's own expected mean that multiplies the day map (like the terrain textures), converging to
  exactly 1 once unresolved, so the hand-off is seamless and orbit stays bit-identical. Weighted by the
  city presence (the night map's lights); the tiled city day texture and the natural close-up textures
  step aside inside cities. First cuts: equal lots / centred square roofs / grey yards / no trees read
  as a board game; a density-only chance of big buildings made half of LA warehouses. +0.55 ms at LA
  from 2 km. Harness: `scripts/city_day.satcmd`.
  **Follow-up (2026-09-29, user review):** trees are individual CROWNS (a jittered tree per 8-m cell,
  2.5-6.5 m, own shade, darker edge, clustered by a 64-m field) — two octaves of value noise were
  same-sized blobs the user saw repeating. Night lamps each have their own type (`cityLampColor`:
  sodium / warm LED / cool LED / halide; LED share 0.25 suburbs .. 0.85 cores, +0.25 on arterials, a
  regional bias), rare neon signs on arterials, traffic lights at arterial crossings cycling with
  `pc.waveTime`; the faintest-lit countryside drew farmsteads (512-m lattice; removed in review 2) not street grids.
  **Farmland** (`farmDayAlbedo`, same ratio-to-mean scheme, stops at a 350-m footprint so orbit is
  bit-identical): where the day map is cultivated (not dark forest, desert, snow, steep, a city, >3 km
  up). Style per ~130-km area: GRID in the Americas (1024-m sections split into 1-4 fields, 10-m gravel
  roads, centre pivots with probability ~dryness^2) or PATCHWORK elsewhere (Voronoi fields stretched
  2.2:1 along the area's direction, hedgerows where green). EVERY rotated lattice is measured from the
  AREA origin (`big * 32 - dAnc`, fixed), never the district origin — that would jump as the observer
  moves; unrotated lattices must divide 4096 m. `cityFrame()` is the shared world-fixed 2D frame.
  +0.35 ms in rural Iowa. Harness: `scripts/rural.satcmd` (rural points: pick them from the night map —
  the first "Iowa" and "Punjab" points were Ames and Ludhiana).
  **Continuity + transition (2026-09-29, user review 3):** nothing in the layout may jump at a district
  border any more. Density is the night map AT THE PIXEL (it was sampled per district), street existence
  is hashed per REGION with even/odd keep probabilities falling with density (no stride switch), lamp-type
  and terracotta biases come from a 4-km value noise (not per-region hashes). Where two DIFFERENT grids
  meet (another region, or an independent district: `CityLayout::differ`) the NIGHT evaluates both grids
  (`cityNightGrid`) and cross-fades them over 80 m (lights add); the DAY does not (an 80-m blend of two
  street grids read as a ghosted double exposure) — it draws a 14-m road along the border instead.
  Low-density blocks become crop fields (`cityFieldP`, up to 80%), and the farm pass cross-fades with the
  same city-presence curve, so cities thin into farmland. Farmland at night got a farmstead FLOOR (removed in review 2)
  (`farmsteadLights` x a map-equivalent 0.004, faded out by a 350-m footprint): the map is ~0 there.
  Farms keep ~0.5 km back from shores and off water-tinted map texels (a lavender field band at Big Sur).
  **Beaches** (`beachAt`): sand 40-140 m wide on low (< ~6 m above the water level), gentle shores, from
  the height function's own shoreline (hand-filtered water map near the observer + `tdShoreOffset`), wet
  near the water; the close-up sand texture there. Harness: `scripts/quickwins.satcmd`.
  **Night v2 — points, not lines (2026-09-29, the user's reference photos).** Averaging an unresolved
  light over its pixel is right in energy but wrong on screen: a real light saturates its own pixel and
  a distant city GLITTERS; the posts-to-lines-to-uniform filtering made far cities flat yellow and the
  street grids too distinct. Now `cityGlitter()`: a world-fixed lattice of 8 m x 2^k cells tracking the
  footprint (cell ~4 footprints, two levels cross-faded, 2x2 nearest cells), a point in 40% of cells
  carrying the cell's light (lognormal weight, mean 1) drawn at ~a pixel, coloured sodium / LED mostly
  and 14% signage (green, blue, red, violet), with a slow sim-time scintillation past ~15 m a pixel.
  Street posts only close up (< ~10 m a pixel: 55% posts, 45% glitter); beyond, arterials only (15%, the
  reference's streaks) + glitter 85%. The night's two-grid border cross-fade is GONE (it doubled the
  cost there; with the streets this faint the borders do not read). Steep ground is 4% lit (the map's
  blur lit the Sandias). Roads: the cell list is 0.125 degrees (43 MB; 0.25 put ~40 segments in an LA
  cell: 0.47 ms -> 0.2). Cost now +1.1 ms at LA 10 km by night (pattern 0.9, roads 0.2), +0.8 by day,
  +0.5 rural. Harness: `scripts/city_lights.satcmd` (incl. Mount Wilson / Sandia Crest low angles).
  **Night v3 (2026-09-29):** the glitter continues at EVERY distance (`cityLightFar`: frame + glitter
  only, past a 400-m footprint; lattice levels to 4-km cells) — the smooth 5-km map read as blobs, so
  orbit views of cities CHANGED on purpose (fields of points); the day side is still orbit-identical.
  Glitter density follows the map (`dens01` = lum / 0.25 sets how many cells hold a point: a suburb is
  fewer points at the same brightness, city edges stay crisp). Points are 0.3-m sources with a star-like
  point-spread (tight core + a 2x halo whose share grows with brightness — the user found uniform discs
  "flat"). Street posts split into HEADS (emission) and POOLS: the pools light the final ground albedo
  (city layout, textures, fields) x the shading normal's up-facing (relief, detail, micro bump) x AO,
  so the street and its texture show in each pool. Close up: posts 70%, glitter 30%.
  **City light sprites (2026-09-29, `city_sprites.comp`, "City light sprites" `clouds.city_sprite_gain`
  1, slot 200):** FAR city lights drawn as SATELLITE POINT SPRITES, so the horizon seen from the ground
  and aircraft is a packed field of crisp points where the surface can only draw the map's blobs (the
  user's intent). After sat_flare.comp, `kCitySpriteLevels` (7) dispatches of a lattice whose cell grows
  with distance (64 m x 2^k cells out to 8 km x 2^k, each cross-faded in over the last quarter of the
  level before: roughly one density ON SCREEN; the last level is 250 cells a side, to 1024 km) put a light in a cell with probability 0.6 x
  the night map's density, a few metres above the DEM (below the sea-level horizon: none), and APPEND a
  finished `GpuSatVisible` to the compact list: effectFlare = 8 x the cell's area / 64^2 at 1 km, 1/r^2,
  the air along the SLANT path (8 km scale height, 40 km sea-level visibility), scintillation, lamp
  colours only (a coloured sprite read as a beacon), warmer than the ground's mix; capped at 8 (no glare
  spikes); gone by 100-150 km of eye altitude (review 2: they were gone by 40 km, so from the
  stratosphere the far towns were the ground's blobs). The point, bloom and glare draws show them with no new
  pipeline. `kCitySpriteMax` (131072) extra slots in `satVisibleBuf` / `satVisibleIdxBuf`, near levels
  first. A city record: slot index 0xFFFFFFFF (picking skips it), `meshPx = -1` (the trail pass skips
  it: `sat_point.vert` skips it when `manualTerrainTest` is 2, the trail pass's value — 1 is the LIVE
  draw at renderScale < 1, and keying on > 0.5 hid every sprite at the Medium preset (0.85); that path's
  manual depth test takes a city light at 0.95 of its range, or the half-res ground hid most of them). The post-flare barrier covers
  `satListBuf` for INDIRECT_COMMAND_READ (the vertex count grows after the post-orbit barrier). Where the
  sprites carry the light the ground glitter keeps "Ground glitter under sprites" (`city_sprite_ground`
  0.6, slot 202, `cityParams.y`) — a point's display saturates, so sprites cannot carry a dense city's
  light per area (gain 12 recovered ~70% at 0.35). **FAR ONLY (review 2: up close they read as
  floating lanterns):** a light is drawn only where a pixel spans more than "City sprites from (m/px)"
  (`city_sprite_start_footprint_m` 25, slot 206, `cityParams.w`) of ground — the SAME stretched footprint
  the ground uses (pixAngle x t / max(|dir . up|, 0.2)), tested per light in the compute
  (`CitySpritePC::pixAng/startFootM`), and the ground's hand-off uses it too (`cFoot`); levels whose
  reach is inside 1.5x the nearest range that can qualify are not dispatched. **Not under Potato** (it skips the
  depth pass whose terrain frame they are placed in: they froze, locked to the observer), and with terrain knocked out
  (Planetarium, bit 1) they sit on the sea-level sphere the ground is drawn as (2026-10-04). **Fog hides
  them** (point cloud occlusion: an LA radiation fog of optical depth ~3 counts as opaque while the
  brighter ground glitter still shows through) — test them with `clouds_v2.fog_amount 0`. Harness
  `state` reports `satellites.visible_count` (the list with them: +36k over LA from 2 km). Cost ~0.
  **User review (2026-09-29, snapshots in harness_runs/fb10):** twinkle 10x slower and a slider ("City
  light twinkle rate" `city_twinkle_rate` 1, slot 201, `cityParams.x`: 0.5-1 rad/s x it); the coloured
  glitter share (signs, traffic lights) only below a 4-16 m footprint; lamp pools light the albedo's
  BRIGHTNESS, not its hue (green suburbs); the city's day base is the map's brightness, not its hue (a
  blue coastal Tokyo); past the march range land is tested by the water map (the city pattern stopped
  on an arc from orbit); the glitter's point-spread is windowed to 0.55 cells (a crosshatch on the 2x2
  choice's switch lines), the arterials' share fades past 40 m a pixel (a milky veil), farmsteads only
  below 30-80 m a pixel (sodium rings round towns from orbit). Farmland needs SETTLED land: the night
  map at mip 5 (~160 km, base removed) 0.0015-0.008 — 0.005-0.05 over farm belts, 0 over the outback,
  the Amazon, the Congo, which the colour test alone drew as fields.
  **Review 2 (2026-09-29, harness_runs/fb11):** NO farm lights at all — the farmstead pattern and the
  farmland floor are deleted ("everybody is not simultaneously blasting their floodlights"; none beats
  few). The procedural lights take the night map's BRIGHTNESS only (the lamps give the colour): the
  map's blue base is uneven, so its subtraction left pure blue/red residue at city fringes, and the
  sparse fringe glitter (x ~25 per point) turned it into vivid coloured dots seen from orbit. **The
  glitter point is ANISOTROPIC** (`gCityViewE`, `gCityFootX`, set before the pattern): sgA along the
  view's ground direction (the stretched footprint), sgX across it (pixAngle x t) — a round point of the
  stretched size drew as a wide horizontal smear at grazing angles, the "gigantic yellow blobs" of far
  towns; now they are dots on screen, sprites or not. The fog's city glow is linear in the lights (a
  full core at 0.2), not `cityBrightness` (saturated by a small town: a tan fog slab over Denver read as
  a dust storm).
  **Review 7 (day at distance):** the tiled city day texture (`cityDayDetailTex`) comes back on the
  day side where the procedural streets stop resolving (weight 1 - strength x (1 - smoothstep(30, 120 m
  footprint))); it was off at every distance with "City street lights" on, and the procedural pattern
  averages to the map by design, so from an aircraft or 40 km a city was the map's smooth 5-km texels.
  `cityDayFar` adds km-scale structure over 80 m .. 4 km footprints (parks on the near layout's void
  field, bright roof zones, a mottle following the footprint; a ratio of mean ~1).
  **Farms (review 8, `farmDayAlbedo`):** rectangular fields from a recursive split of blocks
  (`farmRects`), styles per ~130-km area by region: GRID (the Americas, Australia, some steppe: mile
  sections, quarters, gravel roads, centre pivots where dry, crop rows), STRIPS (Europe, Africa, the Middle
  East, dry Asia: 300-700 m blocks split into strips, orientation per 2-km cell with a track on its edge),
  PADDIES (monsoon Asia where green: small bunded plots, a regional share flooded; `farmWet` adds a
  Fresnel sky reflection and sun glint in the terrain lighting). Replaced a stretched Voronoi patchwork.
  "Ground pattern range (m/px)" (`clouds.ground_pattern_range_m` 800, slot 212, UBO `groundPatternFootM`
  = v1's unread shadowMaxDistM renamed): the farms fade over 0.55-0.9 of it (was a fixed 400).
- **City light LOD settings (review 13, Night lights tab, UBO `cityLod` / `cityLod2`, slots 221-226):** the ground
  footprints (m per pixel) where each layer ends — "Lamp posts to" `clouds.city_posts_footprint_m` 10, "Street grid
  to" `city_grid_footprint_m` 150, "Major roads to" `city_roads_footprint_m` 350 (capped by the layout), "Street
  layout to" `city_layout_footprint_m` 400 (past it `cityLightFar`; the macro and mean ramps scale with it, so the
  pattern's mean still reaches the map's there) — each fading in from ~0.4x its value, and the streets' share of
  the light near / as they fade (`city_street_share_near` 0.7, `_far` 0.15). Defaults reproduce review 12.
- **City light through cloud (review 14):** the cloud blur (`cityLightBlurLod`, by the cloud's opacity in front)
  applies to the FINISHED light (`nightBlurK`, before `tNight`), not to the map the pattern reads: the blurred map
  spread a city's light over the desert beside it and the pattern drew street grids there whenever a cloud was in
  front (west of Phoenix, the grid "turned on" with the eye inside the cloud). The user's tuned LOD values are the
  defaults (posts 27, grid 380, roads 1300, layout 2000 m/px, street share 0 near / 0.13 far, sprite gain 4,
  ground glitter under sprites 1).
- **Regional city styles (2026-09-30, sat_sky.frag `cityStyleWeights`, `kCs*` tables).** Nine styles by
  soft continental boxes with wobbled borders: 0 North America + Oceania, 1 Latin America, 2 Mediterranean
  Europe, 3 Northern Europe, 4 Middle East / N Africa / Central Asia, 5 Sub-Saharan Africa, 6 South + SE Asia,
  7 East Asia, 8 post-Soviet. GEOMETRY is one style per 16-km region, drawn from the weights at the region's
  CENTRE (`cityRegionUp`: the face frame's absolute coordinates are dAnc x 4096 + p2), so a grid never changes
  inside a region: street spacing and range, long blocks (the odd y lines dropped: `CityGrid::oddY`), grids on
  local north (US survey, Chinese plans: the angle of north at the region centre in the face frame), organic
  warp, patchwork share, lots per side, PERIMETER courtyard blocks and SLAB rows — by DISTRICT (`distH`;
  scattered single rings read as picture frames). COLOUR blends per pixel: roof shares (terracotta, light flat,
  blue-green metal, slate + the generic mix, `cityRoofS`), bare / paved / trees, lamp LED share and cool-white
  share (`gCityCool`), coloured-sign multiplier (`gCitySign`). **The day ratio is taken against the pattern
  mean's LUMINANCE where the base is greyed** (`cityDayAlbedo(..., grey)`, `cityDayFar(..., grey)`): per
  channel, the lawn-green mean turned neutral asphalt magenta on the grey base (purple streets at ground level).
  `scripts/city_styles.satcmd`.
- **Terrain-following city limits (2026-09-30, terrain.glsl `cityTerrainLimit`).** Lights and built-up land end
  at a CONTOUR 150-480 m (brighter cores higher, wobbled +-90 m) above the ~40 km mean ground (DEM mip 4.5), and
  none above ~4.5 km, besides the old slope cut; shared by sat_sky.frag (surface: `cityLights` and `cityLum`
  scaled before the layout) and city_sprites.comp (the far points). Rio's peaks and the ranges behind Hong Kong
  and Salt Lake City go dark; the night map's 5-km blur no longer climbs mountainsides.
- **Land at 0 m is terrain (review 9):** a march that ends on the sea sphere over LAND (the water map's
  test at that point) is a terrain hit there, and only water voids a hit (`hitWater`). Land can read 0 m
  (the Lena delta: DEM 0, the water map land), and whether the march landed on the sphere or passed it was
  step luck — terrain shading or the flat sea-level land path: rings about the nadir and z-fighting.
- **Ocean waves ride the world offset (review 9):** `seaMap` reads hitPt.xy + (cloud.pad1, pad2), the
  observer's cumulative east/north motion (CPU double, now from obsDir projected on the previous frame's
  east/north; it was the difference of float degrees, ~1 m steps). The old phase (float lat/lon x R,
  wrapped to the first octave's period only) let the other octaves slide against the terrain.
- Pre-existing bugs fixed on the way: the water mask forced INLAND lakes to sea level (pits under
  Lake Thun, Powell, Titicaca — now only where the DEM is < 160 m, `kWaterMaskMaxM`); terrain normals
  used 21600x10800 texel offsets on the 14999x7500 DEM; the per-pixel jittered march start was the
  salt-and-pepper silhouette speckle.

### Elevation texture encoding — READ THIS BEFORE TOUCHING TERRAIN CODE

**File:** `assets/textures/earth_elevation.png` (R8_UNORM, 14999×7500 — ~2.67 km/texel; older notes said 21600×10800 — land-only DEM)

**The app does not decode this PNG (2026-09-29).** It reads `earth_elevation_0.r8` / `_1.r8` (raw rows,
split under GitHub's 100 MB limit) and `8k_earth_specular_map.r8` through `readRawR8`; the city detail
maps are JPGs. stb_image decoding these PNGs froze the development machine within a few decodes with no
Vulkan involved (docs/FREEZES.md; `UploadStress --decode-only` reproduces it). The PNGs stay the source:
after editing one, run `python tools/make_raw_textures.py` (byte-exact, verified). The PNG path remains
only as a logged fallback when a `.r8` is missing.

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

### The sea is not terrain — the march's `h0 <= 0` gate (2026-09-25) — `sat_sky.frag`

The terrain march can "hit" the sea-level sphere. Over water `tdDemAt` reports `h0 = 0` and nothing the
march adds on top is non-zero (the detail octaves scale with `tdAmp0`'s `smoothstep(0, 80, h0)`), so the
surface such a hit lies on *is* the sphere `tExit` bounds the march by — landing on it is step-size luck:
the ray has to land inside the sub-metre band the march accepts (`minStep` alone is 0.5 m, then the
regula-falsi branch) and beat `tExit` by a fraction of a millimetre. Which rays win flips with distance,
in **arcs concentric about the nadir**: the ocean "rings" (`harness_runs\ocean_rings_sea`, `debugview
normals` over open water — 15.26 % of the frame repainted at 60 m AGL, 0 % at 1200 m). Those pixels drew
flat, detail-free sea-level *land* shading where the ocean (wave) branch belongs.

**Fix:** after the march, `if (terrainH0 <= 0.0) { tHit = -1.0; }`, with every normal/shading consumer
gated on `tHit > 0`. `h0` alone is the test because it is the march's own height function, so the two
cannot disagree, and land cannot read 0 here: the DEM's land baseline (16/255) is 35 m above the sea
baseline (15/255, the table above), and the water mask only forces a sea height below `kWaterMaskMaxM`
(`tdDemAt`, `terrain.glsl`).

**Verified at pixel level** — probes are *not* evidence here (`probe` runs in its own compute shader and
never consumes the fragment-side `tHit` gate, so probe output is byte-identical pre/post): `debugview
normals` repaints **0.00 %** of open water at 60 m AGL afterwards, the beauty diff's `changed_frac`
(0.1526) is exactly the ring pixels, and the 1200 m captures are bit-identical. Land views: v1-v4
bit-identical to the pre-fix baselines, v6/v7 ≈0 %, v8 1.68 % confined to the seaward wedge below the
horizon (that wedge *is* ocean), and v5's 32.6 % was a moved *date* from splitting the script rather than
the fix — see `docs/HARNESS.md`, *Gotchas* → `time sun`.

### The sea (2026-09-30) — `sat_sky.frag` seaHeight / oceanSeaState / shoreSignedDist

- **The wave field is EXACTLY periodic** over (kSeaPeriodX, kSeaPeriodY) = (28571.4, 21428.6) m: the noise
  lattice hashed mod `kSeaCells` (1200), ridges every 6 cells, an INTEGER octave matrix [2 1; -1 2] and integer
  per-octave scales (`kSeaOctA`), arguments reduced mod kSeaCells per octave. The CPU wraps the observer's world
  offset (cityOffsetEast/NorthM, double) into one period (`cloud.oceanState.xy`): seamless, and small at any
  distance flown. Review 9 had fed the waves the unbounded float offset: after a long flight the octave
  arguments (~12x the offset) lost their fraction and the texture broke up (the user's report).
- **Sea state** (`oceanSeaState`, "Sea state from weather" `clouds.ocean_sea_state` 1, slot 213): the wave
  amplitude as a multiple of the old fixed sea from the weather cube (~40 km: storm = cover x convective type
  + rain), the westerly belts and a slow regional wind; choppiness follows. **Whitecaps** ("Whitecaps"
  `clouds.ocean_whitecaps` 1, slot 214): crest foam near (the crest values run ~0.35-0.8, top 5% ~0.7), the mean
  foam fraction (~Monahan) as albedo far, lit as a white diffuser (sun + 0.9 x the sky reflection).
- **Shore** (`shoreSignedDist`, the height function's own waterline): waves shoal to 0.4 over 150 m, breaker
  lines roll in, and the waterline is anti-aliased half from each side (the sea toward wet sand, the beach
  toward a dark water albedo) over a pixel footprint.
- **The wave trace steps to the FIRST crossing** (5 coarse steps, then 6 secant) — the secant alone converged on
  back faces behind crests. **Below-horizon reflections** (a steep facet mirrors the next wave, not the sky) take
  0.2 x the horizon's light (`reflWaterK`). **The sky-reflection march uses u^2 spacing**: 6 uniform samples over
  a ~1000-km grazing path made the extinction far too high and the reflected horizon tan (streaks on every
  steep face — the "tan patches" were pre-existing). The on-screen clouds are composited into the reflection,
  leaning back to the clear march by the cloud's distance (the target lacks the air in front of it).
- **The satellite ocean-glint loop is skipped when its result is zero** (day, or above ~8 km): 512 entries x two
  texture fetches per ocean pixel cost 6 ms of the sky pass from orbit (the `water_map` knockout "saved" it only
  because it turns the sea into cheaper terrain).
- Debug views 40-45 (harness `debugview oceanrefl|oceanfresnel|oceanstate|oceansurf|oceannormal|oceanshore`).
- **Wave normals are filtered to the pixel footprint ALONG the view** (review 14, `gSeaFootM`, seaHeight's `lodW`:
  an octave fades as its cell nears 2 footprints, octave 0 included). A rough sea under a low Sun aliased into
  glittering pixel noise that crawled with any camera motion (the 80th-power diffuse term and the below-horizon
  reflection test amplify any normal noise); the far band's frame-to-frame change at 860 m/s fell from ~11-26 to
  ~0-5 levels. The removed slope roughens the reflection instead (`seaRough`: the reflected ray tilts up, half of
  it the next wave's water, the reflected clouds blur) or the filtered far sea was a mirror. The reflected clouds'
  weight is taken per texel by opacity (the filtered alpha mixed in the no-cloud marker: hard blocks).
- **Water map from Natural Earth 10 m** (`tools/make_water_map.py`, 2026-09-30): land + lakes rasterised at 16K
  between 60 S and 72 N, the distance field averaged down to the 8K texels (sub-texel zero crossing; ~1 km vs
  the 5-km mask's ~2.5 km). Outside the band the 8K mask (ice shelves). Reads the .r8 copies, never the PNGs.
- **Review 16:** waves at `kSeaSpeed` 0.75 (the ~107-m ridges ran at ~27 m/s; a real 107-m wave at ~13), and a base
  ROUGHNESS from the sea state that grows toward grazing views (the unmodelled wind ripples, Cox & Munk), whatever the
  footprint: within ~8 km a calm sea mirrored clouds and the Milky Way crisply up to the horizon. It drives the same
  terms as the footprint's lost slope (reflected ray tilted up, darker, clouds blurred) and the Milky Way reflection's
  mip (3 + 4 x roughness).
- `scripts/ocean_v2.satcmd` (long travel + beach + sea state + aurora ground).
- **Review 22:** **the Sun glint is a Beckmann microfacet lobe** in this pass's units (pi L / E): slope variance
  = Cox & Munk (0.003 + 0.00512 W, W = 7 m/s x the sea state) + 0.06 x the footprint filter's lost slope
  (`seaRoughFoot`) + the Sun's disc; Schlick F (F0 0.02), Smith (Walter's rational fit); x the Sun's REAL
  transmittance (`sunTransMax`: sunSpecTint is hue-normalised). The Phong lobe it replaced peaked at ~0.6 of a white
  surface and narrowed as the waves were filtered flat — no glitter path (snap 9). **Foam, surf and wet sand are
  diffusers**: the Sun on the facet + the terrain's sky irradiance (`skyAmbientTerrain` x 0.4 x "Terrain sky light")
  + the Moon; they took the MIRRORED sky (reflColor) — whitewater showed a reflected image ("mercury").
  **Under an overcast** the reflection's clear-march share (the air in front of reflected clouds, and the sky off
  screen) is x `ovcK` = mix(0.35, 1, the sea point's cloud shadow) where the reflection has cloud or is off screen;
  the on/off-screen fade spans 30% past the edge (8% drew dark wedges in the sea's lower corners under a sunset-lit
  deck). Snap 5's sea had been brighter than the overcast over it (debug view 50: full shadow, 0.48 clear march).

### Satellite ocean-glint gain / floor (2026-09-26) — `sat_sky.frag`, `OceanGlintBuf`

A satellite's mirror flare reaches the water twice: the point sprite/bloom/glare the observer sees
directly, and the specular hit `sat_flare.comp` appends to `OceanGlintBuf` that `sat_sky.frag`
composites onto sea-level hits. Only the first had controls, so "a mild satellite lights up the water"
could only be answered by dimming the satellite itself (all of it shares
`brightnessScale`/`flareGlowGain`). `GpuCloudParams` grew 688 → 704 for two floats, and
`oceanGlintPad0/1` are the load-bearing std140 rounding (C++ packs the pair at 696). The pair first
shipped defaulted to the constants that were hardcoded at the use site (gain 1.0, floor 2.0) so an
unchanged `settings.json` rendered exactly as before; later the same day the defaults were moved to the
tuned values below.

| Field | Ocean tab slider | settings key | Default | Effect |
|-------|------------------|--------------|---------|--------|
| `oceanGlintGain` | "Ocean flare refl" (idx 110) | `ocean_glint_gain` | 0.020175438 | multiplier on the whole glint contribution (0 = none) |
| `oceanGlintMinFlux` | "Flare refl floor" (idx 111) | `ocean_glint_min_flux` | 37.894737 | drop an entry whose `effectFlare` (`.w`) is below this |

The floor is the surgical one: the entry is skipped before any per-pixel work, so faint satellites stop
appearing on the water while a spectacular mirror flare is untouched; the gain scales both alike. They
were written as `if (flux < 2.0) continue;` and a bare contribution term in the sea-level composite
until this change — `sat_flare.comp`'s own write threshold (`OCEAN_GLINT_THRESH`, 1.0) means the list
never carries a lower `effectFlare`, so a floor of 0 is simply "draw everything the list holds".

**Defaults are the author's tuned values** (second half of 2026-09-26): both literals in
`SatelliteSim.h` are float32-exact copies of the pair in `build-win-release/Release/settings.json`
(`0.02017543837428093` / `37.894737243652344`, i.e. the UI's `0.02` / `37.9`), so a first run or the
`SatelliteSimFresh` build reproduces the tuned look rather than the old hardcoded one. The trailing
digits are slider-drag noise, not intent — read them as "~2 % of the original glint term" and "~60 % of
`fIntens`' range". Nothing changes for the tuned file itself (it carries both keys), and nothing in
`applyGraphicsPreset`'s table owns either field, so no preset re-decides them.
`tools/settings_defaults_diff.py <settings.json>` lists every key in a settings file that differs from
the compiled-in defaults — `build-win-release/Release/settings.json` currently differs on ~38 others,
all older tuning or runtime state (camera, window geometry, active tab).

**Latent OOB found on the way:** `hovPhotoMinus/hovPhotoPlus/draggingPhoto` were `[33]` while the
Photometry table's idx range already reached 34 ("Glare near gain" 33 / "Glare near range (km)" 34 —
added after the `[33]` resize and never followed up). An idx-33/34 write therefore landed in the *next*
array's `[0]`: the near-glare rows' hover flags fought the first rows' (`hovPhotoMinus[33]` →
`hovPhotoPlus[0]`) and `draggingPhoto[33]` → `hovCloudMinus[0]`, so their drag lock could grab the wrong
slider. All three are `[35]` now — same class of silent-array bug as the `[33]`→`[46]`→`[112]` cloud
history, and the reason this pair of sliders was slotted at free idx 110/111 rather than beside
"Ocean MW refl" (renumbering a table means renumbering all four shared arrays in lockstep).
`tools/check_cloud_params.py`: 118 fields, identical order.
