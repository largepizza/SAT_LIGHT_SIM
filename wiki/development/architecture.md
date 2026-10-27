# Code architecture

How the C++ code is organised: the three layers, the interface a simulation implements, what happens in one
frame (with two frames in flight), video recording, how SatelliteSim is split across files, how satellite indices are
laid out, the loading screen, and which work runs off the main thread.
The rendering passes themselves (what each shader computes and in what order) are described in
[Rendering](../rendering/index.md).

## Three layers

| Layer | Files | Role | Changes |
|---|---|---|---|
| Platform | `src/VulkanContext.h/.cpp` | instance, device, swapchain, render passes, depth buffer, the frame slots (command buffers, fences, semaphores) and the helpers for memory the CPU and GPU share, timestamp queries, pipeline cache, and small helpers | rarely |
| Framework | `src/App.h/.cpp`, `src/Simulation.h`, `src/UIRenderer.h/.cpp`, `src/AudioSystem.h/.cpp`, `src/Harness.h/.cpp`, `src/FrameCapture`, `src/VideoEncoder`, `src/VideoRecorder`, `src/Log`, `src/Paths` | the window, the frame loop, UI rendering, audio, the harness's command language, frame readback and video encoding | occasionally |
| Simulation | `src/simulations/SatelliteSim*` and its helpers | everything the app actually does | constantly |

`main.cpp` parses the harness flags (they decide where logs and settings live), initialises the log, changes
the working directory to the executable's directory (assets are loaded with relative paths), and constructs
`App` with one `Simulation`. Switching simulations is a one-line change there.

`Paths::exeDir()` is where the read-only game data lives. `Paths::userDataDir()` is where settings, logs,
snapshots, issue reports, bookmarks, caches and exports are written. In order:

1. A harness run's own run folder (`Paths::setUserDataDirOverride()`).
2. The folder named in `<exe dir>/user_data_dir.txt`, when that file exists. Development build trees write it
   (`cmake/EnsureUserDataDir.cmake`) to point at `<source>/user_data/`, so every tree shares one settings file
   and nothing is lost when a build tree is wiped ([Building](building.md)). It is not in the release package.
3. The executable's directory when it is writable (a portable, self-contained install).
4. The per-user directory: `%APPDATA%\SatLightSim` on Windows, `$XDG_DATA_HOME/SatLightSim` or
   `~/.local/share/SatLightSim` on Linux, `~/Library/Application Support/SatLightSim` on macOS.

## The `Simulation` interface

`src/Simulation.h` is the contract between `App` and a simulation. The main hooks:

| Hook | Called | Purpose |
|---|---|---|
| `init(ctx)` | once, after Vulkan is ready | create every GPU resource |
| `onResize(ctx)` | after a swapchain rebuild or a switch to or from the photo target | rebuild what depends on the extent (graphics pipelines bake the viewport; compute pipelines do not) |
| `buildUI(dt, ui)` | every frame, between `UIRenderer::beginFrame()` and `record()` | declare the Clay layout; read input |
| `recordCompute(cmd, ctx, dt)` | every frame, before the render pass | simulation update and all compute and offscreen work |
| `recordPrePass(cmd, ctx, dt, imgIdx)` | after compute, before the render pass | offscreen work blitted into the swapchain image |
| `activeRenderPass(ctx)` | before the render pass | `ctx.renderPass` (clears colour) or `ctx.renderPassLoad` (keeps what the pre-pass blitted) |
| `recordDraw(cmd, ctx, dt)` | inside the main render pass | the scene's draws; `App` owns begin and end |
| `wantsPreUiCopy()`, `recordPreUiCopy(cmd, ctx, image)` | after `recordDraw()` | when true, `App` ends the main pass, lets the sim copy the frame before the UI (a clean video recording, the harness's settle probe) and draws the UI in the load variant of the pass |
| `recordScreenshotCopy(cmd, ctx, image)` | after the render pass ends | copy the finished frame to a staging buffer if a screenshot is pending; also the frame's per-slot readback copies |
| `finalizeScreenshot()` | after the slot's fence wait, once the copy's frame has completed | read that staging buffer back |
| `photoScaleRequest()` | each frame | a factor > 0 makes `App` render into an offscreen target that many times the window size |
| `onKey`, `onChar`, `onCursorPos`, `capturesKeyboard()` | input callbacks | `capturesKeyboard()` true keeps `App` from closing the window on Esc |
| `setAudio(audio)`, `setWindow(window)` | once after init | |
| `frameDt(realDt)`, `wantsQuit()` | each frame | harness hooks: a fixed time step, and leaving the loop when a script ends |
| `virtualCursor(x, y, lmb)` | before `beginFrame()` | the gamepad's virtual cursor overrides the mouse for the UI pass |
| `targetFpsCap()`, `consumeSwapchainRebuildRequest()` | each frame | frame limiter and present-mode changes |
| `setBootStatus()` / `bootStatus(line)` | during `init()` | the loading screen ([below](#the-loading-screen)) |
| `prepareBootChooser(ctx, spec)`, `setBootChoice(option, askAgain)` | before `init()` | the startup graphics chooser ([Controls and settings](controls-and-settings.md#first-run-and-startup)) |
| `prepareUI(ui)` | once, just before the main loop | load what the first frame's UI needs (the icon atlas) |

## One frame

The app keeps **two frames in flight**: the CPU records frame *f* while the GPU still runs frame *f* - 1, so a frame
costs the larger of the CPU and GPU time rather than their sum. The GPU itself still runs the frames one after another
(each frame's GPU work opens with a full barrier), so only memory the CPU touches needs care: buffers the CPU rewrites
are mirrored and copied in at the start of the frame's GPU work, staging and readback buffers exist once per frame
slot, and GPU results read on the CPU are two frames old. The rules are on
[GPU conventions](gpu-conventions.md#frames-in-flight); `SATLIGHTSIM_FRAMES_IN_FLIGHT=1` runs a single frame.

```mermaid
sequenceDiagram
  participant App
  participant Sim as SatelliteSim
  participant GPU
  App->>GPU: beginFrameSlot(): wait for this slot's previous frame (two frames back)
  App->>App: resolve that frame's timestamps
  App->>Sim: finalizeScreenshot()
  App->>Sim: photoScaleRequest() (switch to/from the photo target)
  App->>GPU: acquire swapchain image (not in photo mode)
  App->>Sim: frameDt(), then UI beginFrame
  App->>App: AudioSystem::update() (playlist)
  App->>Sim: buildUI()
  App->>GPU: beginFrameGpu(): the barrier against the previous frame
  App->>Sim: recordCompute()
  App->>Sim: recordPrePass()
  App->>GPU: begin render pass (activeRenderPass)
  App->>Sim: recordDraw()
  App->>Sim: recordPreUiCopy() (only when wantsPreUiCopy())
  App->>App: UI record (skipped for a clean screenshot or a photo)
  App->>GPU: end render pass
  App->>Sim: recordScreenshotCopy() (and the frame's readback copies)
  App->>GPU: recordFrameUploads(), then submit the uploads and the frame together
  App->>GPU: present
```

In SatelliteSim:

- **`buildUI()`** starts by publishing the previous frame's CPU timings (`beginCpuFrameTiming()`), then runs
  the harness (`harnessTick()`) and the cinematic player (`cineTick()`), so a scripted command's effect is in
  the frame it ran in. Then it builds the HUD and windows and handles camera look and picking.
- **`recordCompute()`** reads the latest resolved GPU timings, polls the gamepad, applies movement, advances sim
  time and updates the Sun, Moon, observer frame, stars, planets and ambience on the CPU. It then records the
  GPU work: the satellite mesh pass, the shared scene-depth pass, the orbit and photometry compute passes, the
  cloud passes, the flare and glare passes, trails and the model viewer. The order and the data flow are on
  [Rendering](../rendering/index.md).
- **`recordPrePass()`** renders the background offscreen when it needs to (temporal anti-aliasing, or a render
  scale below 100%) and blits it into the swapchain image.
- **`recordDraw()`** draws the background (sky, terrain, sea, cloud composite), then satellite points, stars,
  planets, the bloom composite, glare and trails.

`App` writes GPU timestamp slots 0 (frame start), 7 (after `recordDraw()`) and 8 (after the UI); SatelliteSim
writes slots 1 to 6. The slot table lives in `VulkanContext.h`; see [Profiling](profiling.md#gpu-timestamp-buckets).
Every `App` phase is a section of the frame profile (`app.wait_fence`, `app.build_ui`, `app.record_compute`,
`app.uploads`, `app.submit`, `app.present`, ...), which is how a slow frame is attributed
([Profiling](profiling.md#frame-profile-and-hitches)).

While the HQ photo target is active, `App` neither acquires nor presents. A few times a second it presents a
progress frame instead (`App::photoProgressFrame()`): it blits the photo image down into a swapchain image and
draws the UI over it, so a long export shows progress rather than a frozen window.

## Video recording

Video is recorded by three framework classes and one sim file:

| Piece | Role |
|---|---|
| `src/FrameCapture.h/.cpp` | asynchronous readback: a ring of host-cached buffers the frame's own command buffer copies the image into (blitted to the output size when it differs); a slot is read only once `completedFrame()` has passed its frame |
| `src/VideoEncoder.h/.cpp` | NV12 (BT.709, limited range) to an H.264 `.mp4`: Windows Media Foundation's sink writer (a hardware encoder where the driver has one, else the software one; nothing is bundled), else an `ffmpeg` on `PATH` fed raw NV12 |
| `src/VideoRecorder.h/.cpp` | the clock, the worker thread that converts and encodes, the statistics |
| `src/simulations/SatelliteSimVideo.cpp` | the player's recording, the REC badge, the harness's settle probe |

**The frame thread never waits.** A frame with no free readback slot is dropped and its time given to the next
capture, which is written repeated, so a video keeps the clock it was recorded on (wall time when playing, the
fixed step in a harness run). The lossless mode (the harness's `path play ... video=` and the Cinematics window's
export) instead requests each frame and holds the path back briefly until a slot is free.

A clean video while the player sees the UI: `wantsPreUiCopy()` makes `App` end the main pass after `recordDraw()`,
call `recordPreUiCopy()` (which leaves the swapchain image in `TRANSFER_DST_OPTIMAL`) and draw the UI in the load
variant of the pass. A video with the UI is copied in `recordScreenshotCopy()` instead. The HQ photo target is never
recorded. The harness's `wait settle auto` reuses `FrameCapture` as a quarter-size probe of every settle frame
before the UI ([Automation harness](harness.md#flow-and-time)).

Recording costs about nothing on the frame: at 1920 x 1009 on an RTX 3070 Ti the frame time and the GPU move by
0 to 0.5 ms. The worker's NV12 conversion takes about 3.7 ms and the hardware encode about 0.8 ms per 1920 x 1008
frame.

## SatelliteSim's files

`SatelliteSim` is one class (`src/simulations/SatelliteSim.h`) whose member functions are spread over several
source files by subject:

| File | Owns |
|---|---|
| `SatelliteSim.cpp` | `init()` and resource creation, texture loading, `recordCompute()`, `recordPrePass()`, `recordDraw()`, positions, beams readback, the playlist (`setAudio()`), screenshots and photos, key handling |
| `SatelliteSimUI.cpp` | all Clay UI: HUD panels, the tabbed settings window, the satellite info and 3D view windows, text fields; settings persistence (`loadSettings()`, `applySettingsJson()`, `buildSettingsJson()`); graphics presets; perf snapshots and the knockout sweep |
| `SatelliteSimCloudsV2.cpp` | the volumetric clouds: init-time noise and weather bakes, the clouds UBO, the per-frame cloud passes, weather evolution |
| `SatelliteSimHarness.cpp` | every harness command (`harnessExec()`), the state JSON written beside captures, the console |
| `SatelliteSimAmbience.cpp` | the ambience's context drivers, tonality, thunder, the music fades ([Sound](../sound/index.md)) |
| `SatelliteSimCinematic.cpp` | cinematic playback and export |
| `SatelliteSimTutorial.cpp` | the first-run tutorial |
| `SatelliteSimBootChooser.cpp` | the startup graphics chooser and the deferred full-sky pipelines |
| `SatelliteSimBookmarks.cpp` | bookmarks and their thumbnail atlas |
| `SatelliteSimReports.cpp`, `SatelliteSimReportsWindow.cpp` | issue reports: shots, the note bar, the window, the export ([Issue reports](issue-reports.md)) |
| `SatelliteSimLightning.cpp`, `SatelliteSimRain.cpp` | lightning channels and the rain particles |
| `SatelliteSimLaunches.cpp` | launches: the schedule, trains and retirees, rockets, plumes, pads, ships, crew flights, mission milestones |
| `SatelliteSimMissions.cpp` | the Missions window |
| `SatelliteSimKesslerTracker.cpp` | the Missions window's Kessler tracker (counts, the debris globe, the latest breakups) |
| `SatelliteSimWeather.cpp`, `SatelliteSimWeatherWindow.cpp`, `SatelliteSimWeatherFeatured.cpp`, `SatelliteSimWeatherCyclones.cpp` | the storm objects' per-frame upload and indices, tornadoes; the Weather window, its radar, featured events and named storms |
| `SatelliteSimWildfire.cpp`, `SatelliteSimHaboob.cpp` | wildfires (points, flames, smoke, burn scars) and haboob dust walls |
| `SatelliteSimRefraction.cpp` | the atmospheric refraction table round the eye ([Refraction](../rendering/refraction.md)) |
| `SatelliteSimVideo.cpp` | video recording and the harness's settle probe |
| `SatelliteSimKessler.cpp` | the Sun's Fury: the background worker, the main thread's mirror of its output, uploads, dust clouds and re-entry fireballs |
| `UIPalette.h` | the UI colour palette and themes shared by the UI files |

Helpers with their own classes: `SatModel` (geometry, materials, attitude, lobe bake), `SatPhotometry` (the CPU
photometric evaluator), `SatMesh` / `SatMeshRenderer` / `SatEnvProbes` (satellite meshes, the model viewer,
environment probes), `SatBench` / `SatBenchmark` / `SatTrace` (benchmarking and traces), `Ambience`,
`Cinematic`, `SpaceWeather` (the aurora's activity as a function of time), `Launches` (the launch catalogue,
booster and upper-stage tracks, orbit raising and exhaust parcels), `Kessler` (the debris world: breakups, the
collision engine, the event log), `Weather` and `StormClimatology` (the storm catalog: storms, lines, cyclones as
pure functions of sim time), `Biome` and `Wildfire` (the land-cover map and the fires) and `Refraction` (the ray
tracer behind the refracted Sun). `SatModel`, `SatPhotometry`, `SatBenchmark`, `SatBench` and `SatTrace` have no
Vulkan dependency, which is what lets `SatModelTool` compile them without the app.

## Satellite indices

Every per-satellite GPU record (`GpuSatOrbit` in the orbit buffer and the per-satellite words attached to it) is
addressed by one index space, in three blocks:

| Range | What |
|---|---|
| `[0, activeSatCount)` | the roster: every satellite of every enabled constellation, in load order |
| `[activeSatCount, poolIdx0())` | `kLaunchCap` (131 072) launch records: raising trains, retiring satellites, and a few reserved slots for rockets drawn as meshes and the selected or followed transient ([Launches](../simulation/launches.md)) |
| from `poolIdx0()` | the debris pool of the Sun's Fury ([The Sun's Fury](../simulation/kessler.md)) |

The CPU mirror `satOrbits` holds the same layout. Code that loops over constellations must loop over
`activeSatCount`, never `satOrbits.size()`, and must not assume that an index past the roster is debris; empty
launch slots carry `constIdx = UINT32_MAX`. Rockets, ships and re-entry fireballs are not records at all: they are
transients, listed each frame by the code that draws them.

## The orbit epoch and its rebake

Each orbit record stores its phase at an **orbit epoch**, and the shader adds the mean motion times the time since
that epoch. The epoch moves every 7 sim-days so that the elapsed time stays small enough for a float. The rebake is
a GPU dispatch, `sat_rebake.comp`: it rewrites every record's phase, tumble phase and (for sun-synchronous planes)
node from epoch-free per-satellite bases (`satPhaseBaseBuf`, 12 bytes each) in exact fixed-point turns, so the
result matches the CPU's double formula to float rounding at any date. Its CPU side is the `orbit_rebake` CPU
bucket. See [Orbits](../simulation/orbits.md).

## Runtime roster changes

`rebuildRoster()` rebuilds every satellite and the per-satellite GPU buffers after the list of constellations
changes (the harness's `roster remove` and `roster add`). It waits for the device to go idle, drops everything
keyed by satellite index (the selection, follow, Track, the 3D view, environment probes, the beam light tracking,
the mesh candidates) and restarts the Sun's Fury, whose world indexes the old roster.

A constellation keeps exactly the same satellites when others are removed or added, because each draws its random
numbers from its own stream (`SatRng`, an emulation of the MSVC C runtime's `rand()` generator, so the layout is
also the same on every platform). The stream's start state is recorded on the first build; a constellation added
later seeds from a hash of its name. Only the satellites' indices move.

!!! warning "Invariant"
    `buildOrbits()` must draw only from its constellation's `SatRng`, never from the global `rand()` or a shared
    stream: a shared stream reshuffles every later constellation whenever an earlier one changes, and the cached
    debris cascade (keyed by the roster) would not match the new layout.

## The loading screen

Initialisation takes seconds (textures, satellite models, noise bakes, pipelines). Instead of a blank window,
`App::run()` creates the UI **before** `init()`, with its pipeline built against `ctx.renderPassBoot`, a third
variant of the main render pass with the same attachment formats (so it works with the same framebuffers).
It installs a callback with `setBootStatus()`. SatelliteSim calls `bootStatus("<step>")` before each step of
`init()` (and per texture and per satellite model); each call appends a line, logs
`boot: <step> (<ms since launch>)` to `satlight_log.txt` and presents one frame (`App::bootFrame()`).

!!! warning "Invariant"
    `bootStatus()` presents a frame, so it may only be called from the main thread during `init()`.

After `init()`, `App` waits for the device to go idle and then rebuilds the UI pipeline against
`ctx.renderPass` (`UIRenderer::rebuildPipeline()`). The wait matters: the last loading frame may still be
executing with the old pipeline, and destroying a pipeline a pending command buffer uses is undefined.

Boot frames reuse the frame loop's command buffer, fence and semaphores but never touch the timestamp slots.
Key and cursor callbacks are ignored until `init()` returns (Esc still closes the window); a resize during
`init()` is deferred to one `onResize()` after it. Text fades on a perceptual curve (alpha =
brightness^2.2), because the UI blends linearly into an sRGB swapchain. Setting the environment variable
`SATLIGHTSIM_BOOT_SCREEN=0` restores a plain launch with no loading screen.

The `boot:` lines in the log are the first place to look when startup slows down.

## The pipeline cache

`VulkanContext` keeps its own pipeline cache, `pipeline_cache.bin`, next to the executable (or in the user
data folder if that is not writable). Every `vkCreate*Pipelines` call passes `ctx.pipelineCache`. It is loaded
at device creation, saved right after `init()` (so a crash or a killed run keeps what init compiled) and at
exit, and discarded once it passes 256 MB. The driver's own cache has a size cap shared by every build of the
app; once it is full, every launch would recompile every changed shader. With the app's cache a warm start is
a few seconds.

`VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR` is requested only during the harness's `shaders reload`
(`ctx.capturePipelineStats`), because it keeps a pipeline out of the caches.

## Threads

The renderer and the simulation run on the main thread. Work moved off it:

| Thread | Work |
|---|---|
| miniaudio's audio thread | mixing; synth voices render there and read their parameters once per 64-frame block |
| ambience voice creation | a voice is made on a worker the first time its layer becomes audible; it plays once made, at the gain set meanwhile |
| music analysis worker (`music::Library`) | analyses the soundtrack at startup, cache first; synchronous in a harness run so the key is deterministic |
| bulk export worker | the photometry bulk export (Settings, Photometry tab), over snapshots of the type and orbit data |
| screenshot encoder | PNG encoding of a screenshot or capture; the next `finalizeScreenshot()` joins the previous encode |
| video worker (`VideoRecorder`) | converts read-back frames to NV12 and feeds the encoder ([Video recording](#video-recording)) |
| log writer (`Log::lineDeferred()`) | writes and syncs log lines a slow frame would otherwise pay for (the hitch lines, the launch jobs' and tornado probes' messages) |
| model bakes at startup | the satellite, rocket, pad and ship models are tessellated and baked on up to 15 threads, largest first, their log lines written in roster order afterwards |
| `parallelFor()` | short-lived threads that evaluate the satellite mesh instances' photometry and poses each frame, and build new launch batches in parallel (at most about 8 ms of building a frame) |
| the launch batch builder | after a time jump of an hour or more, after a walk with many batches missing, or past 1 h/s of warp, builds the missing launch batches in chunks at below-normal priority on all cores but two; the main thread moves each finished prefix into the cache within about 2 ms a frame |
| Missions window refresh | builds the batches the Missions window's rows need on a worker of its own; the rows wait for it |
| plume parcel jobs (`std::async`) | a launch's exhaust parcels, integrated from 2 minutes before lift-off; at most four in flight |
| the debris cascade worker (`KesslerWorker`) | the Sun's Fury collision engine, at below-normal priority, simulating a minute of the current time warp ahead of sim time; the main thread mirrors its output within a budget of a few milliseconds a frame, and sim time holds at the mirrored frontier |
| debris restart, dispose and retire threads | rebuilding the debris world after a breakup that has no rollback point, freeing a replaced world, freeing the GPU buffers a pool growth retired |
| the storm catalog's thread pool | a persistent pool (3/4 of the hardware threads, at most 12; `SATLIGHTSIM_STORM_THREADS` overrides it) that builds missing storm spawn bins, squall-line bins and cyclone seasons after a time jump, lists towers and shields into their bins each frame, and builds a wildfire's front points; the results are joined in a fixed order, so they are identical to a serial build |
| the storm envelope index | in time warp, the weather bake's envelope index is built on threads of its own while the main thread records the frame, and joined before the frame's uploads |
| the Weather window's event search | the forward search for the featured events runs on a second ("scout") storm catalog on a worker while the window is open; the main catalog is never asked about other times, since that would evict the bins the renderer needs |
| wildfire world scan | the fires burning worldwide, for the Weather window's cards |
| harness watchdog | hard-exits a harness run past its `--timeout` |

## Where in the code

| File | What |
|---|---|
| `src/main.cpp` | flag parsing, log, working directory, simulation choice |
| `src/App.cpp` | `run()`, `drawFrame()`, `bootFrame()`, `bootStatus()`, `photoProgressFrame()`, GLFW callbacks |
| `src/Simulation.h` | the interface |
| `src/VulkanContext.h/.cpp` | platform layer, frame slots and the shared-memory helpers, timestamp slots, `beginPhotoTarget()`, pipeline cache |
| `src/FrameCapture.*`, `src/VideoEncoder.*`, `src/VideoRecorder.*`, `src/simulations/SatelliteSimVideo.cpp` | video recording |
| `src/Paths.cpp` | `exeDir()`, `userDataDir()` |
| `src/simulations/SatelliteSim.h` | the class, all members and GPU struct mirrors, `kLaunchCap`, `poolIdx0()`, `SatRng` |
| `src/simulations/SatelliteSim.cpp` | `buildOrbits()`, `uploadSatOrbits()`, `rebuildRoster()` |
| `shaders/sat_rebake.comp` | the orbit epoch rebake |
| `src/FrameProfile.h` | the per-frame section profile ([Profiling](profiling.md#frame-profile-and-hitches)) |
