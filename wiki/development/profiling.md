# Profiling

How to find out where frame time goes: the GPU timestamp buckets, the CPU frame buckets, the knockout bits
that switch individual shader blocks off, the automated knockout sweep, perf snapshots and their analysis
script, and the harness commands for timing and for comparing shader variants reliably. Measured costs of
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
`gpuMsSmoothed[]` an exponential moving average (factor 0.1) for display in Settings, Display, *GPU frame
breakdown*. A pass skipped in a frame writes its slot anyway, so its bucket reads 0 rather than stale data. The
slot table in `VulkanContext.h`, `updateGpuTimingStats()`, the UI labels and the snapshot's JSON keys must change
together.

Setting `SATLIGHTSIM_NO_GPU_TIMERS=1` disables the query pool entirely (every bucket reads 0), for drivers on
which timestamp queries are themselves costly.

Frame time is capped by vertical sync; GPU timestamps are not. A GPU total under the refresh interval does not
show up as a higher frame rate.

## CPU frame buckets

The CPU counterpart, in Settings, Display, *CPU frame breakdown*:

| Bucket | Covers |
|---|---|
| `build_ui` | the Clay layout for the HUD and windows |
| `update_positions` | Sun, Moon, planets, the observer frame |
| `beam_readback` | reading back the beam list, clustering, the ground-spot top-K |
| `update_stars` | the star catalogue's positions and suppression chain |
| `light_pollution_dome` | the light-pollution dome's sectors |
| `update_planets` | the planets' render entries |
| `other` | the wall-clock frame time minus the GPU total minus everything above: present and vsync wait, driver submission, App-side work, any CPU block without a bucket |

Timers are scoped (`SatelliteSim::CpuTimer`, an RAII object that adds to its bucket, so one bucket can be timed
at several sites). `beginCpuFrameTiming()` must stay the **first statement of `buildUI()`**: it publishes the
previous complete frame and clears the accumulator, so it never publishes a half-filled frame. The resulting
one-frame lag matches the GPU buckets', so a sample of both comes from the same frame.

A large `other` is a finding: it is fixed per-frame cost that this table does not look at.

## Knockout bits

`debugDisableMask` is a bit mask that switches off individual shader blocks or whole passes, each with a fallback
that is safe for the rest of the frame (for example, skipping the terrain march leaves the same "no hit" value
the shader already produces for sky). With mask 0 rendering is unchanged. The mask rides in the `CloudParams`
uniform block (`cloud.dbgDisableMask`) and, for the satellite point shader, in its push constants.

The table is `kDebugToggles[]` at the top of `SatelliteSimUI.cpp`: one row per bit with a display label and a
stable JSON key, which is also the harness's `knockout` key. A row there adds a checkbox in Settings, Display,
*Knockout profiling* and a step in the sweep. `kDebugToggleSlots` in `SatelliteSim.h` must equal the row count
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

Two more bits are pipeline swaps, not in-shader branches, and are set by presets rather than swept: 262144
(`potato_sky`, the minimal sky shader) and 524288 (`lite_sky`, the `SKY_LITE` variant). See
[Weak-hardware tiers](../rendering/hardware-tiers.md).

A knockout skips *execution*, not compiled code: a block that is switched off still occupies registers. A bit
can also make another pass more expensive (for example, with the scene-depth pass off, nothing occludes the
cloud march), which is why a cost can come out negative.

## The automated knockout sweep

Settings, Display, *Run knockout sweep* (or the harness's `sweep`) walks the whole table: a baseline at mask 0,
then one step per bit. Each step holds its mask for 6 discarded frames (`kSweepSettleFrames`) and then averages
the **raw** GPU buckets over 24 frames (`kSweepSampleFrames`). It takes about 15 s and appends one record with
`"record_kind": "knockout_sweep"` to the snapshot log, holding every step's full bucket breakdown and its
`cost_ms` against the baseline.

Two choices make it trustworthy where a hand measurement is not:

- It reads raw values, not the smoothed ones. The display average takes about 40 frames to settle, so a reading
  taken soon after flipping a checkbox blends two configurations.
- It **pauses sim time** for its duration (restoring the previous state afterwards), so every step measures the
  same scene. Otherwise satellites move, beams re-target and clouds drift during the sweep, and the per-bit
  differences mix cost with scene change. The user's own mask is also saved and restored.

`cost_ms` is not clamped at zero; a negative value is information.

## Perf snapshots and `analyze_profile.py`

*Save Snapshot* (Settings, Display, or F9) appends one JSON record (`"record_kind": "snapshot"`) to
`perf_profiles/profile_log.jsonl` in the user data folder: the GPU buckets, the CPU buckets, resolution,
observer position and altitude, sim time, the knockout mask, the GPU name, the quality settings and preset, a
`beams` block (active beams, ground spots, whether the beam rays are drawn), the full settings and the view
(observer direction and height, camera, the cloud map's drift). The file is JSON Lines, so it grows by appending
across sessions. A snapshot doubles as the way a location is handed to the harness (`snapshot` command,
[Automation harness](harness.md#snapshots)).

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
2. For the cloud march, build the variants' SPIR-V (`cmake --build build --config Release --target
   CompileShaders`) and load each with `shaders reload march=<spv path>`, which rebuilds the cloud pipelines from
   the files on disk without a relaunch. Copy the SPIR-V into `build/Release/shaders/` by hand: the full build's
   post-build copy fails while the app holds the files open.
3. **Check the register count before timing.** `shaders reload` reports the driver's statistics for the
   rebuilt pipeline (register count, binary size, spill memory) through `VK_KHR_pipeline_executable_properties`.
   On NVIDIA the cloud march compiles either to 128 registers or to about 227, and the second is 20 to 60% slower
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
| `src/simulations/SatelliteSim.cpp` | `updateGpuTimingStats()`, `beginCpuFrameTiming()`, the timestamp writes |
| `src/simulations/SatelliteSimUI.cpp` | `kDebugToggles[]`, the breakdown panels, `savePerfSnapshot()`, `buildPerfSnapshotJson()`, `startKnockoutSweep()`, `updateKnockoutSweep()` |
| `src/simulations/SatelliteSimHarness.cpp` | `perf`, `sweep`, `shaders reload`, `knockout` |
| `tools/perf_analysis/` | `analyze_profile.py`, `live_perf_table.py` |
