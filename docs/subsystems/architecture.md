# Architecture

The three layers, frame loop order, unified scene depth, the shared depth buffer, shader #include headers, loading screen, startup graphics chooser, VulkanContext helpers, frames in flight, persistent settings, fixed sim state.

Part of the subsystem notes (`docs/subsystems/`, map in `AGENTS.md` §4). Moved verbatim from
`CLAUDE.md` on 2026-11-03. Do not read this file whole: `python tools/outline.py docs/subsystems/architecture.md`
lists its sections and topics with line numbers. New notes go next to the section they belong to.

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
                           dispatch 1: scene_depth.comp   (twice: a quarter-res seed pass, then the
                                                            half-res shared terrain/ocean/MESH depth —
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
                           dispatch 4: the cloud bucket   (recordCloudsV2: weather evolution, lightning,
                                                            v2 march (+ adaptive tiles / pass B), resolve,
                                                            far layer, as needed; then cloud_march.comp:
                                                            half-res composite + aurora/airglow-red +
                                                            beam rays + per-pixel cloud ground shadow)
                           dispatch 5: sat_flare.comp     (INDIRECT; photometry in place on the
                                                            compact list: sky/extinction/pollution +
                                                            visibility culling + sprite size)
                           city_sprites.comp              (far city lights APPENDED to the same list)
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
sim->recordPrePass(cmd)  → the sky TAA (jittered sky pass → sky_taa.comp resolve, also the temporal
                           upscaler below 100%) → blit into the swapchain; or, TAA off / SKY_LITE below
                           100%, the low-res sky → vkCmdBlitImage. Nothing at 100% with TAA off
vkCmdBeginRenderPass     → owned by App
sim->recordDraw(cmd)     → sky/ground background (the mesh is a surface in it, Phase 4c) →
                           satellite points → stars → planets → flare composite → glare
                           (satellite sprites + mesh glints) → trail composite
ui.record(cmd)           → Clay → Vulkan quads/text/icons on top
vkCmdEndRenderPass       → owned by App
  (while auto exposure is on, App ends the pass after recordDraw: recordPreUiCopy takes the meter's 64x36 blit of the
   frame WITHOUT the UI, then the UI draws in the load variant of the pass)
sim->recordScreenshotCopy() → the UC6 screenshot / bookmark-thumbnail copy when one is pending (and the meter's blit on
                           the HQ photo's frames, which draw no UI)
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

**Meshes in it, and what the half-res composite means over them (2026-10-21, issue 20261021_174219, a Starship
caught at its tower at night in fog).** A texel takes the nearest mesh distance over its full-res pixels AND a one-pixel
ring round them (`meshDistAt`: an open lattice left whole texels in its holes, marched past the tower beside texels
marched to it — the truss was a field of odd squares). So a mesh texel's composite holds only what is in FRONT of
the mesh, its neighbours what is behind as well, and every full-res consumer must test per texel: `sat_sky.frag`'s
joint-bilateral upsample always runs on a mesh pixel and drops each texel whose composite distance is past the mesh
(one filtered alpha averaged a near fog texel with a far cloud bank, called both "behind" and drew a dark half-res
staircase along every mesh edge); `lightning.frag` mode 2 lights cloud / fog on a mesh pixel only in front of the
mesh and ends its haze there; `cloud_march.comp` drops a RESOLVED v2 cloud whose distance is past the texel's surface
(the resolve's history kept the cumulus behind a moving hull as dark blocky patches; the march clamps only new samples).

**That drop is for MESH texels only (2026-11-12, issue_20261112_215754 "Black line on sky terrain intersection").** The
depth pass marks a texel whose surface is a mesh with `.g` = 3 (`scene_depth.comp`; `sat_sky.frag` reads `.g` as the
terrain's shadow only up to 1.5, so 3 is "not computed" there). On terrain and the sea the resolved distance is no
evidence of stale cloud: it is the opacity-weighted mean of the 3x3 NEW samples round the pixel (`cloud_v2_resolve.comp`,
review 20c), so at a ridge against a hazy sky the crest's texels carried the sky samples' dust distance (~25 km at
opacity ~0.55, against ~2.5 km at ~0.1 for the ground's) while their surface was 5 km out. The drop removed the dust IN
FRONT of the ridge (the march had clamped it correctly) and the combine pass (DB.a -1) gave the full-res pixels round it
none: a 1-2 px dark band along every silhouette over haze, ~10 levels for the first ~60 frames after a history reset and
~24 once the still view settled (~120 frames on). Knockout `fog_layer` / `dust_amount 0` removed it; the AP and limb
volumes, the sky TAA, the sparse grid, the full-res grid and the history depth test did not. On a mesh that same mixing is
what makes the drop work in motion (the hull's own fresh samples hold no cloud, the samples beside it the cloud behind).
Dead end: filling a dropped terrain texel from its same-surface neighbours that are not behind theirs held only for the
first ~60 frames (0 levels), then its neighbours were "behind" too (16-19 levels); on a hull it drew the limb's haze as
orange blotches (issue_20261110_184611's path).

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
  swapchain, so linear alpha steps look far brighter than intended. The boot UI's scale is
  clamp(H / 900, 0.8, 1.6) (0.8 on a short window since the SDF font; it was never below 1 while the font was a
  bitmap that broke up under ~16 px).
- `SATLIGHTSIM_BOOT_SCREEN=0` (`run.py --boot-screen off`) restores the old white-window launch.
- **The app keeps its own pipeline cache** (`VulkanContext::pipelineCache`, `pipeline_cache.bin` next to the exe, else
  the user data folder; loaded at device creation, saved after init and at exit, dropped past 256 MB; every
  `vkCreate*Pipelines` passes `ctx.pipelineCache`). The driver's Vulkan cache is capped (~512 MB on NVIDIA, one file
  shared by every build of the app): it filled on 2026-10-01 and stopped storing, and every launch recompiled every
  shader changed since — 30-40 s to "Starting" (pipelines 18 s, the clouds' bakes 4 s, then a frozen first frame).
  With it: 4.6 s on a warm cache. `VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR` is requested only during harness
  `shaders reload` (`ctx.capturePipelineStats`). Look at the `boot:` lines of satlight_log.txt when startup slows.
- **Boot timeline (2026-10-21, RTX 3070 Ti, warm caches): ~8.6-9 s to "Starting"** — textures ~1.5 s, clouds v2 0.1 s,
  constellations + the 142 satellite model bakes ~3.5 s, satellite orbits/pipelines 0.3 s, meshes/probes/settings/
  launches/Kessler ~1.5 s, the first-frame prewarm ~0.5 s, the music analysis (harness: synchronous) ~1.3 s. It was
  ~15 s: the model bakes (tessellation, lobe bake, the brute-force validator, occlusion, OBJ export) ran one after the
  other, ~10 s with the rocket, capsule, pad and station models. They run in parallel now (`loadConstellations`: up to
  15 worker threads, largest first, `bakeModelType(..., logOut)` collecting each model's log lines, written in roster
  order after; the main thread shows "Satellite models k / N"). A cold pipeline cache adds ~35 s (Pipelines) and ~8 s
  (clouds v2) once per build directory.
- **The first frame does no one-off work any more (2026-10-21): ~360 ms -> under 30 ms.** `setAudio` (still in the
  loading screen) also runs `launchPrewarm` (the planes and every batch the first walk wants, in parallel: ~0.4 s),
  the deployment era's roster upload, the city-sprite and fireball pipelines (made lazily before), the half-res targets
  at the loaded render scale, the first joystick query, and logs the music analysis's messages (every `Log::line` is
  flushed to disk: a few on one frame cost ms each). App rebuilds the swapchain there if init asked
  (`consumeSwapchainRebuildRequest`), and `applyFpsCapMode` asks only when the present mode CHANGES
  (`VulkanContext::presentModeBuilt`): it asked on every launch, and that rebuild (swapchain + every size-dependent
  pipeline, ~215 ms) was most of the first frame. `Simulation::prepareUI(ui)`, just before `mainLoop`, loads the icon
  atlas.
- **Music analysis cache key = a content hash** (kAnalysisVersion 2): the write time changed with every build's copy of
  the music, so every launch after a rebuild re-analysed all tracks (~10 s).

## Startup graphics chooser (2026-10-06)

The user: a window to choose Planetarium or Full graphics "before even loading in shaders, sat shapes", toggleable
like the intro, "so we do not just freeze up someone's PC on bootup", and Potato if Planetarium is too much.
`App::runBootChooser` (App.cpp) draws it on the loading screen right after the title frame and BEFORE
`sim->init()`: only the UI pipeline (built against `renderPassBoot`) exists, so it is cheap on any GPU. The sim's half
is `SatelliteSimBootChooser.cpp` through two `Simulation` hooks: `prepareBootChooser(ctx, spec)` (what to offer and
pre-select; returns whether to show it) and `setBootChoice(option, askAgain)`.
- **Options**: 0 Full graphics, 1 Planetarium, 2 Potato (a smaller card below). Pre-selected: the saved preset's mode
  for a returning player, else the device recommendation (`deviceRecommendedBootMode`, a wrapper over
  `seedGraphicsPresetFromDevice`: integrated / CPU / virtual GPU -> Planetarium) — one tier lighter after a crash
  (the `session.lock` sentinel), with a note. "Full" keeps a full tier (or Custom) already saved, else
  `recommendedFullPreset` (the device seed, or Low on an integrated GPU). Mouse, keys (arrows / Tab / 1-3, Enter or
  Space) and gamepad (d-pad / left stick, A or Start; X toggles "ask again"). **No timeout**: it never continues
  into Full on its own. Closing the window there quits without initialising the sim.
- **Persisted** as `display.ask_graphics_mode` (absent = true, so first runs and upgrading players see it once; the
  chooser's "Ask on every startup" box and the Display tab's STARTUP toggle set it). A crashed last session shows
  it regardless. Shown only with the loading screen on.
- **Read before init**: `peekBootSettings()` parses settings.json (`display.ask_graphics_mode`, `graphics_preset`,
  `debug_disable_mask`) and checks the crash sentinel BEFORE init recreates it. The pick is applied after
  `loadSettings` (`applyBootChoice`) and replaces the crash path's forced Planetarium; the intro's UC1 benchmark
  does not re-decide a launch where the player picked.
- **Tier pipeline deferral (`fullSkyDeferred`)**: init knows the tier at its first line (the pick, else the saved
  preset, else the crash path / device seed). On a light tier `createSkyBgPipeline`, `createSkyLowResResources`
  and `createSkyTaaResources` skip the three FULL `sat_sky.frag` pipelines (inline sky, low-res prepass, SKY_TAA)
  — at init and on every resize. `ensureFullSkyPipelines()` (top of `recordCompute`, after the fence: same
  destroy+create as `onResize`) builds them the first frame `fullSkyNeeded()` — no light-sky knockout bit, or a
  render scale below 100% (the low-res prepass has no lite variant). **Not deferred** (still built for every tier):
  the SKY_ENV / SKY_REFL variants (env probes, mesh sharp reflections), the cloud v1/v2 march compute pipelines and
  their noise / weather bakes, the textures and satellite models. Any new pipeline built from the full
  `sat_sky.frag` must follow the same flag or the light tiers compile it again.
- **Safety net** (`updateBootSafetyNet`, first thing in buildUI after the CPU timer): after ~2 s and 5 frames of
  warm-up, the median of the next 10 wall-clock frames (or of at least 3 once they add up to 5 s); above 250 ms it steps one tier down (a full tier ->
  Planetarium, Planetarium -> Potato), once per launch, with a 12-s banner (`graphicsAutoNoticeText`). Off in a
  harness run unless `SATLIGHTSIM_BOOT_SAFETY_MS` (run.py `--boot-safety-ms`) sets the threshold.
- **Look**: the HUD palette (`UIPalette.h` `Pal` / `Style`, included by App.cpp): near-black `panelSolid` cards with
  the faint `Style::borderColor` rule, the red accent (`btnAccent`) for the selection border, the checkbox and
  Start, `Style::windowCornerRadius`, the amber note on `rowHighlight`. The first cut had a soft blue theme of its own
  (2026-10-06, the user: "no soft blue UI windows").
- **Harness**: skipped (a run never waits for input) unless `SATLIGHTSIM_BOOT_CHOOSER=pick=N` (run.py
  `--boot-chooser N`), which scripts it in four frames; `--boot-capture` captures them. `state` -> `render.boot_choice
  / full_sky_deferred / full_sky_pipeline / ask_graphics_mode`; `tools/harness/scripts/boot_chooser.satcmd`.
  `SATLIGHTSIM_BOOT_CHOOSER=0` hides it, `=1` shows it whatever the setting.
- The tutorial's Graphics card (`TUT_GRAPHICS`) still appears on Planetarium / Potato: it explains there are no
  clouds and where the preset is.

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
- **Two frames in flight** (2026-10-25, see "Frames in flight" below; `SATLIGHTSIM_FRAMES_IN_FLIGHT=1` = the old
  single frame, the A/B): per-slot command buffers, fences and acquire semaphores; the GPU still runs the frames one
  after another
- `VK_ACCESS_SHADER_READ_BIT` + `VK_PIPELINE_STAGE_VERTEX_SHADER_BIT` for compute→vertex SSBO barriers (not `VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT`)
- Compute→compute SSBO barriers use `VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT` on both sides
- `onResize` must recreate graphics pipelines (viewport baked in); compute pipelines are viewport-independent
- `glowBuf` is device-local; the peak flare for the magnitude UI comes from its per-slot readback copy
  (`rbGlow`, `recordFrameReadbacks`), the frame from two frames ago

### Frames in flight (2026-10-25, branch perf-frames)

The CPU records frame f while the GPU runs frame f-1, so a frame costs max(CPU, GPU) instead of CPU + GPU.
`kMaxFramesInFlight` = 2, `framesInFlight` from `SATLIGHTSIM_FRAMES_IN_FLIGHT` (1 or 2, default 2: one build A/Bs both).
`App::drawFrame` starts with `ctx.beginFrameSlot()` (waits only for this slot's previous frame; returns whether there
was one, gating its timestamps and screenshot), records, then `ctx.recordFrameUploads()` and submits the slot's upload
command buffer and main command buffer as two `VkSubmitInfo`s (`markSlotSubmitted`).
- **GPU frames stay serial.** Every frame's GPU work opens with a full memory barrier (ALL_COMMANDS -> ALL_COMMANDS,
  MEMORY_WRITE -> MEMORY_READ | WRITE: the upload command buffer's first command, and `beginFrameGpu` in the main
  one), so device-local resources (cloud histories, TAA, depth, every compute output) need nothing: frame f's GPU work
  sees frame f-1's whole result, as before. **Only memory the HOST touches changed:**
- **`mirrorUpload(dst, mem, size, whole, name)`**: a buffer the CPU rewrites each frame becomes a device buffer + a
  CPU shadow (the returned pointer: code keeps writing it as it wrote the mapped memory); `recordFrameUploads` copies
  the shadow through a per-slot ring into the device buffer at the start of the frame's GPU work. `whole` = the whole
  buffer every frame (small UBOs: CloudParams, the cv2 UBO, light dome, point style, mesh frame UBOs); else only the
  ranges the code marks with **`mirrorDirty(base, off, len)`** (merged; `mirrorDirtyAll`): the satellite type header
  every frame, the LUT / types / lobes / occluders / samples in `uploadSatOrbits`, the storm buffer's sections, the
  haboob grid rows that changed, bolt segments, the fireball list, mesh instances and the shadow accel per slot.
  **A new CPU write into a mirrored, non-whole buffer must mark its range, or the GPU never sees it.**
- **`uploadPatch(dst, off, data, len)` / `uploadFill`**: scattered writes into a GPU buffer (the tornado probe
  request, the eval counter reset, the Sun-volume level clears) without a shadow.
- **`slotted(&buf, &mem, &ptr, size, usage, props, name)`**: one copy per slot, the bound variables swapped at
  `beginFrameSlot` — staging buffers (Kessler, launches), the UI's vertex / index buffers, host readback targets
  (`terrainFrameRead`, `glareRead`, `pickedVisible`, `glintRead`, the exposure meter, the cv2 flash buffer's head).
  A descriptor set must never bind a slotted buffer (it changes every frame): slotted buffers are copy sources /
  destinations and vertex input only.
- **`readbackCreate(size, &ptr, name)`**: a per-slot HOST_CACHED copy of a device buffer, filled by a copy recorded in
  `recordFrameReadbacks` (glow, reflect beams, beam glow dome, the flash buffer's head); the pointer reads the slot's
  copy, i.e. the result of the frame that last used this slot (two frames old, it was one).
- **`stagingAlloc(bytes)`**: per-frame staging for image uploads recorded into the frame (the radar, the Kessler
  tracker globe, the cyclone track map: their CPU images are plain memory now).
- **`deferDestroy`**: a buffer the frames in flight may still use is destroyed once its slot comes round again.
- **What remembers which frame it read is per slot**: the beams' observer basis (`lastBeamObsDirS/EffHS/RadiusS[slot]`,
  rebasing the beam list into this frame), the lightning list's observer direction, the tornado probe ids, the glint
  list's validity, the cloud pass timer's stamps, the exposure meter's pending flag. A readback with companion state
  stored once would pair the result of frame f-2 with the state of frame f-1.
- **One-shot readbacks wait for their frame**: `ctx.completedFrame()` = the newest frame known finished; the radar,
  the harness probe, the photometric check and the screenshot copy record the frame index they were requested in and
  read when `completedFrame() >= it`. `finalizeScreenshotCopy(true)` drains instead: `path play record=` and the
  cinematic export need exactly one rendered frame per recorded frame.
- **Descriptor writes and recreation drain**: every `vkUpdateDescriptorSets` outside init goes through
  `frameSafeUpdateDescriptorSets` (drains the frames in flight when any are, counts it), and runtime resource
  recreation (render-scale targets, the low-res sky, `ensureFullSkyPipelines`, swapchain) calls `drainFrames()`. In a
  steady frame there are none: harness `perf` -> `frames_in_flight` {upload_kb_per_frame, patch_kb_per_frame,
  copies_per_frame, drains, descriptor_writes, one_time_submits} shows it (steady: ~400 KB uploaded, 0 drains, 0
  descriptor writes, 0 one-time submits). A drain or descriptor write every frame is a bug: it silently restores the
  single frame.
- **Timestamps**: the query pool holds `kTimestampCount x kMaxFramesInFlight`; a slot writes and resolves its own range.
- **Measured** (RTX 3070 Ti, 1920x1009, uncapped present, `tools/harness/frame_bench.py`: the matrix suite's scenes,
  stills, time running and every player-speed motion path; two runs a side, `harness_runs/fb/`): wall-clock frame
  summed over the stills 839 -> 767 ms (-8.6%), over the motion paths 1404 -> 1330 ms (-5.3%); orbit -7..-16%, a
  launch -13..-15%, 1 h/s -10..-15%, the eclipse -11%, ground views -2.5..-6%. Mode 1 of the new build matches the old
  build (-0.9 / -0.6%). The GPU's own time is ~2% higher with two frames (it no longer idles between frames, so it
  boosts less), outweighed by the overlap. Matrix suite mode 1 vs 2 in one build: every noise, motion and grain number
  equal within 0.01-0.1 levels.

---

## Subsystem: Persistent Settings

`settings.json` is written next to the exe on settings-window close and on `cleanup()`, loaded in `init()` after `initConstellation()`.

Persisted fields: photometry params, `ui_scale`, settings window position, audio volumes, camera orientation (`az_deg`, `el_deg`, `fov_y_deg`), observer lat/lon, time scale index, keybindings (action → GLFW key code), constellation `enabled` + `highlight` state per name.

If the file is missing (first run) all defaults are used silently.

**The compiled defaults ARE the shipped look (2026-10-20).** A packaged build has no settings.json, so a fresh install is
the member initializers in SatelliteSim.h + the first-run preset (`recommendedPresetForDevice`). On 2026-10-20 the
author's tuned settings (`user_data/settings.json`, preset Custom) were baked into the initializers (2 s.f.): clouds_v2
sun gain 2.3, multi-scatter 0.27, history weight 0.046, light march 2500 m, mid layer density 0.3, cloud sunlight Rayleigh
0.86, accumulation 39, adaptive rate / sparse-when-still OFF (adaptive parallax 1.6); flat layers coverage 0.57 / sun 1.8;
sea far ripple 1.7, ripple size 1.5, wave sharpness 0.1; the aurora sheet / storm / coverage tunables, shimmer 0;
extinction 0.18, mirror boost 1000; display automatic render scale OFF, clouds follow render scale OFF. A discrete GPU
with >= 5.5 GiB now starts on High (= those defaults, 100% render scale). Check with `python tools/settings_defaults_diff.py
<settings.json>` (it now also reads `settingClamp(...)`-wrapped loads and lists values outside their load range). Out of
scope and left alone: `kessler.dust_gain` (the author runs x1000, default x100), `launches.after_storm`,
`display.photo_scale`, `clouds.aurora_kp_manual` (unused while space weather is automatic).

---

## Fixed Simulation State

**Start time**: UTC 2036-11-22 02:06:22 (`kInitWholeSec`; the cloud drift's reference epoch `kCloudDriftEpochS`
is still 2036-06-21 00:00:00 = J2000 s 1,150,891,200). Sim time is real UTC (the Earth rotation angle is GMST).
**Observer**: 67°S 67°W → ECEF `obsDir = {0.1527, -0.3596, -0.9205}`, facing north — this is only the
compiled-in fallback used before `loadSettings()`/the intro cinematic override it; see below for
where the observer actually starts in practice.

---
