# Profiling

How to find out where frame time goes: the GPU timestamp buckets, the CPU frame buckets, the frame profile
that explains a single slow frame, the knockout bits that switch individual shader blocks off, the automated
knockout sweep, perf snapshots and their analysis script, and the harness commands for timing and for comparing
shader variants reliably. Measured costs of
particular features are on their own pages (for example [Rendering](../rendering/index.md)); this page is about
the instruments.

## GPU timestamp buckets

`VulkanContext` owns a timestamp query pool with 9 slots (`kTimestampCount`). Slots are written at points in
the frame; the difference between consecutive slots is a bucket. With one frame in flight, `App::drawFrame()`
resolves the previous frame's queries right after its fence wait, with no stall.

| Slot | Written after | Writer |
|---|---|---|
| 0 | frame start | `App` |
| 1 | the satellite mesh pass and the shared scene-depth pass | `SatelliteSim::recordCompute()` |
| 2 | (a marker just before the orbit pass; its bucket reads about 0) | `recordCompute()` |
| 3 | the orbit pass (`sat_orbit.comp`) and the per-beam cloud occlusion pass | `recordCompute()` |
| 4 | the cloud passes | `recordCompute()` |
| 5 | flare photometry, bloom and glare compute, trails and the model viewer | `recordCompute()` |
| 6 | the sky background draw (in the pre-pass or the main pass, whichever drew it) | `recordPrePass()` or `recordDraw()` |
| 7 | the rest of `recordDraw()`: satellite points, stars, planets, composites | `App` |
| 8 | the UI | `App` |

`updateGpuTimingStats()` turns the slots into eight buckets: `scene_depth`, `beam_cloud_block` (the vestigial
slot-2 bucket, about 0), `orbit_compute`, `cloud_march`, `flare_compute`, `sky_background_draw`,
`satellite_star_draw`, `ui_overlay`, plus the total. `gpuMsRaw[]` holds the last frame's raw values and
`gpuMsSmoothed[]` an exponential moving average (factor 0.1) for display in Settings → **Performance** →
*GPU frame (ms)*. The Performance tab is one of the advanced tabs, shown once *Show advanced settings* (Display
tab) is on; it holds every instrument on this page that has a UI. A pass skipped in a frame writes its slot anyway, so its bucket reads 0 rather than stale data. The
slot table in `VulkanContext.h`, `updateGpuTimingStats()`, the UI labels and the snapshot's JSON keys must change
together.

Setting `SATLIGHTSIM_NO_GPU_TIMERS=1` disables the query pool entirely (every bucket reads 0), for drivers on
which timestamp queries are themselves costly.

Frame time is capped by vertical sync; GPU timestamps are not. A GPU total under the refresh interval does not
show up as a higher frame rate.

## CPU frame buckets

The CPU counterpart, in Settings → Performance → *CPU frame (ms)*:

| Bucket | Covers |
|---|---|
| `build_ui` | the Clay layout for the HUD and windows |
| `update_positions` | Sun, Moon, planets, the observer frame |
| `beam_readback` | reading back the beam list, clustering, the ground-spot top-K |
| `update_stars` | the star catalogue's positions and suppression chain |
| `light_pollution_dome` | the light-pollution dome's sectors |
| `update_planets` | the planets' render entries |
| `orbit_rebake` | the 7-sim-day orbit rebake's CPU side (the work itself is a GPU dispatch); at a fast time warp it runs every few frames |
| `kessler` | the debris cascade's main-thread side: taking the background worker's output, the uploads, the dust and fireball lists (UI label *Debris cascade*) |
| `launches` | the launch schedule, the trains' and retirees' orbit records, rockets, plumes and pad lights |
| `other` | the wall-clock frame time minus the GPU total minus everything above: present and vsync wait, driver submission, App-side work, any CPU block without a bucket |

Timers are scoped (`SatelliteSim::CpuTimer`, an RAII object that adds to its bucket, so one bucket can be timed
at several sites). `beginCpuFrameTiming()` must stay the **first statement of `buildUI()`**: it publishes the
previous complete frame and clears the accumulator, so it never publishes a half-filled frame. The resulting
one-frame lag matches the GPU buckets', so a sample of both comes from the same frame.

A large `other` is a finding: it is fixed per-frame cost that this table does not look at.

The `launches` bucket has its own breakdown: the harness's `launch state` reports `cpu_split_ms` (eased over 30
frames), with the window walk, the live list, freeing, record writes, the visuals, plume builds and sorting,
re-entries and pad lights as separate parts. `tools/harness/launch_bench.py` times fixed launch scenes with launches
on and off ([Automation harness](harness.md#launch-and-mission-checks)).

## Frame profile and hitches

The buckets are averages. A **hitch** is one slow frame, and its cause is usually a one-off: a batch of launches
being built, a buffer reallocation, a large sync from a background thread. `src/FrameProfile.h` keeps, for every
frame of the main thread, named wall-clock sections:

```cpp
FPROF_SCOPE("kessler.sync");   // adds this scope's wall time to the section "kessler.sync"
```

Sections are inclusive (a nested section also counts inside its parent), so read the names as a tree. A call from
any thread other than the main one is ignored, which makes a scope safe in code a worker also runs.
`App::drawFrame()` publishes each frame first thing (`fprof::beginFrame()`) and contributes the `app.*` sections
(`app.wait_fence` is waiting for the GPU; `app.build_ui`, `app.record_compute`, `app.record_draw`, `app.submit`,
`app.present` and the smaller phases between them); `recordDraw()` adds the CPU buckets as `cpu.*`. The cost is a
clock read per scope.

Two more kinds of section help narrow a slow frame down:

- **`app.other`** is the frame's wall time outside every top-level `app.*` section. `App`'s phases are siblings, so
  a hitch whose sections sum to far less than its wall time shows its unexplained part here: time spent somewhere
  nothing is scoped yet.
- **Splits.** `fprof::markReset()` starts a stopwatch and each `fprof::mark("name")` adds the time since the
  previous mark to the section `name`. A long function can be cut into consecutive pieces without nesting scopes:
  `recordCompute()` reports `rc.seg1_pre` through `rc.seg7_flare`, and the debris cascade's sync adds its own
  `kru.*` splits.

**The harness's own work does not count.** Each frame's statistics and the hitch test use its wall time less the
section `harnessTick` (the harness writes and syncs a result line for every command, which can take 10 to 15 ms on
a busy disk; no player pays that). When the two differ, the log line reads `wall N ms, M without the harness`.

A frame longer than the threshold (50 ms; environment variable `SATLIGHTSIM_HITCH_MS`, 0 = off) is logged to
`satlight_log.txt` as `hitch: wall N ms (frame, warp, sim t): <largest sections>` (at most 400 lines a run) and
kept in a ring of the last 256 hitches; the last 4096 frames' times are kept for statistics. The very first frame
after load is not reported, since its interval includes initialisation.

The harness reads them with `hitches` (the frame-time distribution since `hitches clear`: mean, p50, p90, p99, max,
frames over 50, 100 and 250 ms, the sections summed over the hitch frames, and every hitch frame with its largest
sections, worst first) and `hitches threshold <ms>`. `tools/harness/scripts/hitch_ladder.satcmd` steps through every
time rate from 1x to 1 yr/s right after load.

### The hitch check

`tools/harness/hitch_check.py` turns the ladder into a pass/fail regression check. It runs the ladder (every rate
for 180 frames, then `hitches`) in up to four scenarios, one harness launch each:

| Scenario | Starts | Script |
|---|---|---|
| A | right after load | `hitch_ladder.satcmd` |
| B | right after load with a user's `settings.json` (`--settings`, required) | `hitch_ladder.satcmd` |
| C | in 2031, while the deployment era is still filling | `hitch_ladder_2031.satcmd` |
| D | day 32 of the debris cascade after the Sun's Fury | `hitch_ladder_cme.satcmd` |

The default is A, C and D. Each rate's p99 and max frame time are checked against
`tools/harness/hitch_thresholds.json` (looked up as `default`, then the scenario's `all`, then the rate, then the
scenario's rate), and so are the first frame after load (the max of A's or B's 1x step, `first_frame_max_ms`) and
the time from launch to the first frame (the log's `boot: Starting` line, `boot_max_ms`).

| Limit | Value |
|---|---|
| p99 / max, default | 50 / 75 ms |
| p99 / max at 1 month/s | 65 / 95 ms |
| p99 / max at 1 year/s | 75 / 100 ms |
| p99 / max, scenario B (a user's settings) | 60 / 160 ms |
| first frame after load | 90 ms |
| launch to first frame | 18000 ms |

The limits are for an RTX 3070 Ti class GPU at the harness's 1600 x 900; `--scale 1.5` loosens every one of them on
a slower machine. `--run <folders>` judges finished runs without launching, and `--retries N` reruns a failing
scenario and keeps the better result. The exit code is 1 when a check fails.

The app must have the GPU to itself: another app on it (another agent's harness run) adds 20 to 100 ms to frames.
Before each scenario the check waits (up to `--wait-alone`, 1800 s) until no other `SAT_LIGHT_SIM` process runs and
polls while it runs. A scenario that shared the machine is rerun (`--alone-retries`, 2) and otherwise reported
**CONTENDED** rather than judged, with exit code 3.

!!! warning "Invariant"
    Put an `FPROF_SCOPE` on anything new that can take more than about a millisecond in a frame. Work that can
    take longer than a frame's budget belongs on a worker thread or behind a per-frame budget, and one-off work
    belongs in the load, not the first frame. The launch batch builds (about 8 ms of wall time a frame, the bulk after a
    time jump on a background thread), the plume
    parcels, the debris cascade's mirror (3 ms a frame), its reuse-history prune (about 1 ms a frame), the GPU debris
    pool's growth and the ambience voices all work this way; see [Code architecture](architecture.md#threads).

## Knockout bits

`debugDisableMask` is a bit mask that switches off individual shader blocks or whole passes, each with a fallback
that is safe for the rest of the frame (for example, skipping the terrain march leaves the same "no hit" value
the shader already produces for sky). With mask 0 rendering is unchanged. The mask rides in the `CloudParams`
uniform block (`cloud.dbgDisableMask`) and, for the satellite point shader, in its push constants.

The table is `kDebugToggles[]` at the top of `SatelliteSimUI.cpp`: one row per bit with a display label and a
stable JSON key, which is also the harness's `knockout` key. A row there adds a switch in Settings →
Performance → *Knockouts* (it reads **Skip** while the bit is set, **On** otherwise) and a step in the sweep. `kDebugToggleSlots` in `SatelliteSim.h` must equal the row count
(a `static_assert` enforces it).

| Bit | Key | Switches off |
|---|---|---|
| 1 | `terrain_march` | the terrain march in the sky pass |
| 2 | `atmosphere_loop` | the sky pass's atmosphere scattering loop |
| 4 | `sun_optical_depth` | the Sun's optical depth along the light path (`optDepth`) |
| 8 | `ocean_sky_reflection` | the sea's sky reflection |
| 16 | `airglow_red` | the red airglow march |
| 32 | `aurora_curtain` | the aurora curtain march |
| 64 | `cloud_self_shadow_cone` | (no current shader reads this bit) |
| 128 | `reflect_beams_glow_and_spot` | Reflect Orbital beam light: the ground spots, the beam lighting of cloud and the drawn beams |
| 256 | `cloud_shadow_per_pixel` | the per-pixel cloud shadow on the ground |
| 512 | `beam_self_march_dispatch` | the per-beam cloud occlusion dispatch |
| 1024 | `scene_depth_pass` | the shared scene-depth pass (everything then behaves as if no surface occludes) |
| 2048 | `fog_layer` | fog and dust |
| 4096 | `sat_point_cloud_occlusion` | cloud occlusion of satellite points |
| 8192 | `beam_pointing_rays` | the per-pixel beam pointing-ray loop |
| 16384 | `cirrus_march` | the legacy cirrus march (drawn only when the volumetric march is off) |
| 32768 | `volumetric_cloud_march` | the volumetric cloud march (and the far cloud layer) |
| 65536 | `sat_sky_glow_bins` | the sky pass's 64-bin satellite sky-glow loop |
| 131072 | `beam_tile_cull_disabled` | **an optimisation A/B, not a feature**: forces the beam loops' full-buffer scan instead of the per-tile culled lists. The image must be identical; its sweep cost is the saving, with the opposite sign to every other row |
| 1048576 | `sat_part_occlusion` | occlusion between satellite parts (when it is enabled at all) |
| 2097152 | `sat_meshes` | satellite meshes in the main view |
| 4194304 | `water_map` | the water map in the shared terrain height function |
| 8388608 | `mesh_shadow_accel_disabled` | **an optimisation A/B**: the satellite meshes' shadow rays go back to testing every occluder of the model per pixel instead of the posed, Sun-binned occluder grid ([Satellite meshes](../rendering/satellite-meshes.md)). Same image; its sweep cost is the saving |
| 16777216 | `plume_tile_cull_disabled` | **an optimisation A/B**: `cloud_march.comp` walks the whole dust / plume / flame Gaussian list in every texel, without the per-tile cull or the per-texel faint skip ([Launch visuals](../rendering/launch-visuals.md)) |
| 33554432 | `plume_cull_disabled` | **an optimisation A/B**: keeps the plume Gaussians the CPU's significance cull would drop |

Two more bits are pipeline swaps, not in-shader branches, and are set by presets rather than swept: 262144
(`potato_sky`, the minimal sky shader) and 524288 (`lite_sky`, the `SKY_LITE` variant). See
[Weak-hardware tiers](../rendering/hardware-tiers.md).

A knockout skips *execution*, not compiled code: a block that is switched off still occupies registers. A bit
can also make another pass more expensive (for example, with the scene-depth pass off, nothing occludes the
cloud march), which is why a cost can come out negative.

## The automated knockout sweep

Settings → Performance → *Run knockout sweep* (or the harness's `sweep`) walks the table. It measures a baseline
at the **current** mask, then one step per bit that the current mask still renders (a bit the preset already
sets is listed under `already_disabled` and skipped), then the baseline once more. Each step holds its mask for 6
discarded frames (`kSweepSettleFrames`) and then averages the **raw** GPU buckets over 24 frames
(`kSweepSampleFrames`). It takes about 15 s and appends one record with `"record_kind": "knockout_sweep"` to the
snapshot log, holding every step's full bucket breakdown and its `cost_ms` against the baseline, plus
`baseline_end` and `baseline_drift_ms`: when the closing baseline differs materially from the opening one, the
scene moved during the sweep and every cost carries that drift, so retake it.

Two choices make it trustworthy where a hand measurement is not:

- It reads raw values, not the smoothed ones. The display average takes about 40 frames to settle, so a reading
  taken soon after flipping a switch blends two configurations.
- It **pauses sim time** for its duration (restoring the previous state afterwards), so every step measures the
  same scene. Otherwise satellites move, beams re-target and clouds drift during the sweep, and the per-bit
  differences mix cost with scene change. The user's own mask is also saved and restored.

`cost_ms` is not clamped at zero; a negative value is information.

## Perf snapshots and `analyze_profile.py`

*Save snapshot* (Settings → Performance → *Record*) appends one JSON record (`"record_kind": "snapshot"`) to
`perf_profiles/profile_log.jsonl` in the user data folder; every issue report shot (F9, F7) appends the same record
with a `report` key naming its issue and shot ([Issue reports](issue-reports.md)). A record holds the GPU buckets, the CPU buckets, resolution,
observer position and altitude, sim time, the knockout mask, the GPU name, the quality settings and preset, a
`beams` block (active beams, ground spots, whether the beam rays are drawn), the full settings and the view
(observer direction and height, camera, the cloud map's drift). The file is JSON Lines, so it grows by appending
across sessions. A snapshot doubles as the way a location is handed to the harness (`snapshot` command,
[Automation harness](harness.md#snapshots-and-issue-reports)).

```bash
tools/perf_analysis/.venv/Scripts/python.exe tools/perf_analysis/analyze_profile.py [path/to/profile_log.jsonl]
```

The script reports mean pass cost by resolution and per megapixel (a flat per-megapixel cost means a pass is
purely resolution-bound), a resolution ratio at matched altitude (to separate the resolution effect from scene
changes in the same data), knockout costs, Pearson correlations against scene variables, and two plots under
`analysis_output/`. For each sweep record it prints a ranked cost table, with the bucket that moved most for
each bit: a bit can sit in a different pass than expected.

## Measuring with the harness

`perf [frames=60] [name=]` averages the raw GPU and CPU buckets over N frames and reports the GPU total and
wall frame-time distributions (`gpu_total_ms` mean, p50, p90, max); `name=` also appends it to the run's perf
log. `perf` measures a still view. For the cost while moving, `path play` reports `gpu_ms_mean` over the path
and the worst frame's buckets, since spikes hide in a mean.

```
preset Medium
observer lat=61.2 lon=-149.9 agl=10
time sun -12
camera az=0 el=20 fov=80
wait settle
perf frames=120 name=view_medium
knockout +volumetric_cloud_march
wait settle 10
perf frames=120 name=view_medium_noclouds
```

`tools/perf_analysis/live_perf_table.py` tabulates the named `perf` results of a run or a live session,
optionally against a baseline.

## A/B testing a shader change

Run-to-run noise between launches is 5 to 10%, larger than many changes worth measuring. For a reliable
comparison:

1. Keep **one app running** (`live.py start`) and compare variants inside it, interleaved (A, B, A, B), at fixed,
   settled views.
2. For the clouds, build the variants' SPIR-V (`cmake --build build --config Release --target
   CompileShaders`) and load each with `shaders reload march=<spv path>` (also `resolve=`, `lightning=`,
   `composite=`, `layers=` for those passes), which rebuilds the cloud pipelines from the files on disk without a
   relaunch. Copy the SPIR-V into `build/Release/shaders/` by hand: the full build's
   post-build copy fails while the app holds the files open.
3. **Check the register count before timing.** `shaders reload` reports the driver's statistics for the
   rebuilt pipeline (register count, binary size, spill memory) through `VK_KHR_pipeline_executable_properties`.
   On NVIDIA the cloud march compiles either to 128 registers or to roughly 220 to 230, and the second is 20 to 60% slower
   whatever the change was meant to do. A variant that flipped allocation measures the flip, not the change
   ([GPU conventions](gpu-conventions.md#register-pressure-in-the-cloud-march)). `wg=<X>x<Y>` sets the march's
   workgroup size (specialisation constants), which bounds the registers a thread may get.
4. Check images too: capture the same settled views for each variant and compare with `imgtools.py diff`. A
   change that is faster because it draws something different is not an optimisation.

The first launch after shader changes recompiles pipelines and is slow; never read timings from it.

## Where in the code

| File | What |
|---|---|
| `src/VulkanContext.h/.cpp` | the slot table, `resetTimestamps()`, `writeTimestamp()`, `resolveTimestamps()` |
| `src/FrameProfile.h` | `FPROF_SCOPE`, `fprof::mark()`, `fprof::beginFrame()`, the hitch log and ring |
| `src/simulations/SatelliteSim.cpp` | `updateGpuTimingStats()`, `beginCpuFrameTiming()`, the timestamp writes |
| `src/simulations/SatelliteSimUI.cpp` | `kDebugToggles[]`, the Performance tab (`buildSettingsPerformanceTab()`), `savePerfSnapshot()`, `buildPerfSnapshotJson()`, `startKnockoutSweep()`, `updateKnockoutSweep()` |
| `src/simulations/SatelliteSimHarness.cpp` | `perf`, `hitches`, `sweep`, `shaders reload`, `knockout` |
| `tools/perf_analysis/` | `analyze_profile.py`, `live_perf_table.py` |
| `tools/harness/launch_bench.py`, `tools/harness/scripts/hitch_ladder*.satcmd` | launch cost benchmark, hitch ladders |
| `tools/harness/hitch_check.py`, `tools/harness/hitch_thresholds.json` | the hitch regression check and its limits |
