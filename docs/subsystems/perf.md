# GPU performance profiling and perf passes

Timestamps, CPU buckets, knockouts, the sweep, frame profile / hitches, and every perf pass with its measurements.

Part of the subsystem notes (`docs/subsystems/`, map in `AGENTS.md` §4). Moved verbatim from
`CLAUDE.md` on 2026-11-03. Do not read this file whole: `python tools/outline.py docs/subsystems/perf.md`
lists its sections and topics with line numbers. New notes go next to the section they belong to.

## Subsystem: GPU Performance Profiling

Built session 29 to replace guesswork ("N_VIEW is probably the bottleneck") with real
measurement — used to find and fix the terrain step-count bug and the aurora resolution/noise-bake
wins documented under "Active Development" above. Four pieces:

**In-app GPU timestamp queries** (`VulkanContext`): a `VK_QUERY_TYPE_TIMESTAMP` pool whose slot
count has changed several times as passes came and went — `VulkanContext::kTimestampCount` carries
the authoritative slot table and is the only place that mapping is documented. `updateGpuTimingStats`,
`kPerfLabels[]` and `savePerfSnapshot()`'s JSON keys all mirror it and must be updated together.
Each frame slot has its own range of the pool; results are resolved in `App::drawFrame` after
`beginFrameSlot()` waits for the slot's previous frame (two frames back) — no stall. Slot layout is a shared contract: App.cpp writes 0 (frame start), 5 (satellite+star draw
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
16384=`cirrusMarchCS`, 32768=the volumetric clouds (clouds v2: since 2026-10-06 nothing of them is dispatched —
march, resolve, far layer, lightning/rain map, tiles — see "Weak-Hardware Sky Tiers"),
65536=`sat_sky.frag`'s 64-bin satellite sky-glow loop, 131072=**beam tile cull OFF** (not a feature
knockout — an optimization A/B; see "Beam pointing-ray tile culling" below), 1048576=satellite
part occlusion (`sat_orbit.comp`, geometry-model types; via `GpuSatTypeHeader::occlusionOn`),
4194304=the water map in the shared height function (terrain v2 P2, `tdDemAt`; also the cost A/B),
8388608=**mesh shadow accel OFF** (an optimization A/B like 131072: `sat_mesh.frag`'s per-pixel occluder loop),
16777216=**plume tile cull + faint skip OFF**, 33554432=**plume significance cull OFF** (both optimization A/Bs:
cloud_march.comp's dust / plume Gaussian list, see Launches "What the Gaussian list costs"),
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

**Frame profile and hitches (2026-10-20, `src/FrameProfile.h`).** The buckets above are averages; a hitch is ONE
frame, and its cause is usually a one-off (a batch build, a reallocation, a sync of a big batch). `FPROF_SCOPE("name")`
adds a scope's wall time to a named section of the main thread's current frame (inclusive: read nested names as a
tree; calls from other threads are ignored, so it is safe in code a worker also runs); `App::drawFrame` publishes
each frame first thing (`app.*` sections: wait_fence = the GPU, build_ui, record_compute, record_draw, submit,
present...), `recordDraw` adds the CPU buckets as `cpu.*`. A frame over 50 ms (env `SATLIGHTSIM_HITCH_MS`, 0 = off)
is logged to satlight_log.txt as `hitch: wall N ms (frame, warp, sim t): <largest sections>` (400 lines at most a run)
and kept for harness `hitches` (docs/HARNESS.md: wall p50 / p99 / max and the hitch frames since `hitches clear`).
Scoped so far: every per-frame subsystem entry point (launches, Kessler sync / uploads / fills, clouds v2, mesh scene,
ambience, stars, the dome, harness, reports, bookmarks...). **Add a scope to anything new that can take > 1 ms in a
frame.** Benchmark: `tools/harness/scripts/hitch_ladder.satcmd` (every time rate from 1x to 1 yr/s right
after load; its header lists the variants run: the user's settings, 2031, after the CME).
The 2026-10-20 pass, measured this way (RTX 3070 Ti, 1600x900, harness fixed 1/60 s step), and what each fixed —
see the sections named: plume builds (Launches: async), batch builds + pad deconfliction (Launches: parallel, rival
day index, ship landing skip), the Kessler mirror (budgeted sync queue, pool reserved at load, prune throttled, GPU
pool x4 growth), the first frame (ambience + bookmarks loaded during the loading screen).
**Hitch pass 2 (2026-10-21).** `app.other` = a frame's time outside every `app.*` section (a hitch whose sections sum
to far less than its wall was spent unscoped: that is how the first frame's swapchain rebuild was found), new App
sections (`app.poll`, `app.ui_begin`, `app.screenshot_copy`, `app.recreate_swapchain`), and `fprof::mark("x")` splits
(the time since the previous mark; `recordCompute` carries `rc.seg1_pre` .. `rc.seg7_flare`). **The statistics and
the hitch threshold use the frame NET of the harness's own work** (`Frame::netMs` = wall - `harnessTick`; the log line
gives both): every command's result line is written with an fsync, ~10-15 ms each on a busy disk, so a ladder step's
`time scale; time play; hitches clear` frame read 50-60 ms no player ever pays. Fixed: the first frame (see *Loading
screen*), the boot's model bakes, the Kessler mirror's roster / event / stream tail, prune and GPU growth (see
*Kessler*), launch batch builds with a wall-time budget as well as the count cap (pieces sized from the eased cost of
one build, `LaunchState::buildMsEach`, stopping past ~8 ms: the cap follows the LAST frame's cost, and after a frame
of cheap builds 400 expensive ones took 300-600 ms at 1 week/s after the CME), ambience voices on workers, the
Missions window's searches (`launchUpcoming` builds its missing batches in parallel first, and with
`launchUpcomingBudgetMs` = 8, set by a non-forced `missionsRefresh`, stops there, leaves its output as it was and
reports `launchUpcomingIncomplete`; the window keeps its rows and finishes next frame: `msNextPending` / `msPinPending`;
a refresh after a jump was 65-230 ms, bookmark jumps with the window open up to 310 ms, now <= ~65 ms), the ambience
context's first evaluation (`rosterRangePresentFrac` sorts every constellation's deployment-era seconds once: ~50 ms,
now at load), the Kessler worker's publish (outside its mutex; `kesslerSync` try-locks). Still open (QA 2026-10-21):
a player breakup BEFORE the CME restarts the world on the main thread (`kesslerRestart` ~220 ms: `buildWorld`, and
its `launchInitBlock` makes the next walk rebuild the launch planes, ~75 ms); the first walk after a bookmark jump
is ~25-35 ms (the window walk over ~3000 planes plus 8 ms of builds); at 1 yr/s a frame could take ~40 ms of batch building (one rival day bucket, `rivalBucket`, enumerating a site's planes; hitch_thresholds.json allows 1mo / 1yr p99 65 / 75 ms). Since 2026-10-21 a frame that needs fewer buckets than half the cores builds each split over the threads by plane (`rivalBucket(..., parallel)`, per-plane lists concatenated in plane order: same entries, same order); the worst 1-yr/s frames' `launch.buildBatches` is ~20 ms now (hitch_check A: p99 51, max 53 ms, of which ~25 ms GPU). **Since 2026-10-21b the builds after a jump and every build past 1 h/s of warp run on the background build** (CLAUDE.md "Launches" > The background build; the main thread builds none at a fast warp while it runs): hitch_check A / C / D all within limits, C 1yr p99 37.6 / max 38.7 (was 75.2 / 96.7 with main-thread builds of cold rival buckets, 62 ms in one frame), the 1-yr/s hitches left are the Kessler sync (`kesslerSync`, `kru.deaths`). **Hitch pass 3 (2026-10-21c):** the Kessler upload's deaths go up in arrival order, at most 24k and ~2 ms a frame (`kDirtyPoolHead`; it sorted the whole backlog and sent up to 131k records, 6-16 ms), the roster's dirty list merges only what is new (`kDirtyRosterSorted`), the replaced pool buffers are freed on `kRetireThread`, `applyBuildJob` looks at the clock every 16 batches (256 overran its 2 ms to 9-14 ms) after reserving the map once, and the launch walk's full free hands the dropped batches to a detached thread. hitch_check (A / C / D, 1mo / 1yr p99): A 34.8 / 38.4 (was 39.8 / 48.2), C 31.5 / 36.0 (32.3 / 37.6), D 37.5 / 41.7 (35.3 / 62.2; max 91 -> 44); at 1 yr/s Kessler's sync is ~3-6 ms of a frame and the GPU (`app.wait_fence` 25-35 ms) is the rest. New scopes `kessler.retire`, `kessler.showGens`, `kessler.queuedBreakups`.
**Hitch pass 4 (2026-10-24, perf4-cpu: the Watch buttons).** Every Watch clicked through the UI (`ui click`, the user's
settings, `harness_runs/p4c/` in the perf4-cpu worktree: `w3.satcmd`, `parse.py`, `cmp.py`, `steady.py`), worst frame
before -> after: tornado 319 -> 44 ms (the radar pipeline, built by the first radar probe: now in setAudio's prewarm),
the Sun's Fury 101 -> 38 (`Catalog::configure` recalibrated, ~55 ms, though only the env key — the drift — changed: the
calibration is kept when the settings are the same; and the launch builds after the jump: now all to the background
job, and the Missions window's refresh on its own worker, `LaunchState::UpJob`), cyclone row 63 -> 41, hurricane 57 ->
33, waterspout 50 -> 32, haboob 103 -> 71 (now all GPU: `app.wait_fence` 65 ms there); the rest within noise, GPU-bound.
The Weather window's featured search runs on a worker (`wxScanJob`; one step on the main thread was up to ~12 ms
whatever its 2.5-ms budget). The Weather and Cinematics windows capture the mouse (a click there also picked a satellite,
~10 ms). **The hitch log itself was a hitch:** each line was fsynced on the frame thread (`Log::line`), so a run of slow
frames paid ~5-10 ms each for its own line — the `app.other` in the hitch lines (hitch_check D 1d: 6.2 ms a hitch frame
on average, max 22); hitch lines now go through `Log::lineDeferred` (a writer thread), and so do the launch job's and the
tornado probe's log lines. Scopes added: `app.cmd_begin` / `app.cmd_end` / `app.pace`, `ui.seg1..6` (buildUI's splits),
`wxf.*`, `fire.born` / `fire.state` / `fire.smoke` / `fire.front`, `wx.configure`, the Watch functions; `fprof::Scope::end()`
ends a section early. **To profile steady frames:** `hitches threshold 1; hitches clear; wait 120; hitches top=1` records
every frame, and `sections_in_hitches`' sums / frames are the per-frame means (`steady.py`). hitch_check (A / C / D) fails
at 1 h/s - 1 mo/s before and after alike: the GPU (`app.wait_fence` 45-70 ms; the CPU part ~10 ms: Kessler's sync,
the clouds' updates, the launch walk).
**Regression check: `python tools/harness/hitch_check.py`** (scenarios A load, B a user's settings via `--settings`,
C 2031, D day 32 after the CME; `--run <folders>` checks finished runs) against `tools/harness/hitch_thresholds.json`
(p99 / max per rate, the first frame after load, launch to first frame); exit 1 on a regression. **It must run alone
on the GPU**: other agents' harness apps add 20-100 ms to frames, so it waits for other SAT_LIGHT_SIM processes to
exit, polls during the run, reruns a scenario that shared the machine and reports it CONTENDED (exit 3) otherwise.
**Medium storm-view pass (2026-10-23, the user's issue reports 20261023_001522 / _002905 / _003611 / _003455 — tropical
Pacific storms at sunrise from 0.5-11 km, Medium-like settings, render scale 0.67, 1920x1009; `harness_runs/perfmed/`:
`base.satcmd` + `parse.py`, `final.satcmd`).** The frame is GPU + the CPU's serial part: with ONE frame in flight
(`App::drawFrame` waits on the fence first) every ms of `app.record_compute` is a frame ms. Before -> after, uncapped
(`set display.fps_cap_mode 0`, VSync caps the harness at 16.7): frame 17.6-21.5 -> 14.0-18.2 ms, GPU 13.9-17.6 ->
12.2-16.4, cloud bucket 6.1-10.7 -> 5.2-9.6, sky 4.7-8.6 -> 4.5-6.5, `record_compute` 2.4-2.9 -> 1.2-1.8. Captures at the
same frames: mean |diff| 0.3-1.4 levels, within run-to-run noise (`screenshots/overnight_20261023/perf/`).
- **Never loop over mapped GPU memory on the CPU: memcpy it out first.** HOST_VISIBLE | HOST_COHERENT memory without
  HOST_CACHED is uncached (each scalar read a bus transaction, ~70 ns), device-local host-visible memory (the flash
  buffer) is across PCIe (~0.7 us a read): `readExposureMeter`'s 9216 byte reads were 0.67 ms of every frame,
  `updateRainMotion`'s 1028 float reads of the rain map 0.73 ms (now 0.03 / 0.11).
- `updateStars` skips the dome / extinction / sizing of a star below the limb or under a shut day gate (all 9000 drew
  nothing by day: 0.79 -> 0.12 ms) and tabulates the extinction over sin(elevation) (1024 knots, < 0.001 mag).
- Sky pass: the green / sodium airglow's noise only at 62-130 km (the profiles are < 1e-6 of their peak outside; every
  step of every ray paid three noise reads wherever the Sun is under ~8.6 deg: 1.4-2.8 ms); the city glow's per-step
  night-map work skipped while it is composited x 0 (`nightFactor`, the Sun ~5.7 deg up: 0.3-0.4 ms).
- Clouds: the Sun transmittance volume bake on 8 threads a ray (above: lightning pass 1.9-2.2 -> 0.75-0.8 ms with the
  next item); the terrain Sun-horizon map not baked with no terrain of 100 m in its reach (CPU DEM, the lines' box
  across / along the Sun + 60 km) and no land within 60 km (`terrainInSunBox`, `landWithin`; 0.3 ms over open ocean).
- **What is left** (knockouts / settings A/B'd in one live app): the cloud march 3.6-8 ms (Cb towers ~1.1 ms at
  `cb_columns` 2; from inside the high layer at 11 km the cirrus ~4 ms; mid layer 0.7-0.9); the god rays ~1.7-2.5 ms of
  the sky pass + the froxel fill 0.5 ms, less since a follow-up: an atmosphere step whose lowest point is over 3 km above
  the Sun volume's top (or the terrain ceiling, 9.4 km), under a Sun above the sample's horizon (sin > 0.05), takes no
  god-ray reads (they all return 1 there): sky -0.6..-1.2 ms (a1 0.6, b3 0.7, d1 1.15), captures and the HQ photo
  within the gate-on vs gate-on noise;
  full-resolution clouds at Medium +1.5..+4.7 ms (the user's choice, issue 20261022_180607); the atmosphere loop's rest
  ~2 ms (Sun optical depth 0.3-0.5); `view_samples_max` 96 -> 64 only -0.3 ms after the airglow fix (left). CPU: the
  launch visuals 0.35-0.6 ms (far built plumes' per-parcel states), the storm update 0.76 ms a frame while sim time runs
  and `buildStormEnvIndex` 0.9 ms each frame the weather bake runs (every frame at 5 min/s), `thunderStep` 0.22 ms (the
  rockets' retarded-time scan), stars 0.5 ms at dusk / night. The structural item: overlapping the CPU with the GPU
  would hide the whole 1.2-4 ms CPU part — done since 2026-10-25 (two frames in flight, VulkanContext Helpers).
- **The storm content (follow-up, Medium, defaults / the user's settings, `harness_runs/perfmed/storm_def3`,
  `storm_user2`; `mkstorm2.py`).** Frame / GPU / cloud bucket, and the towers' share (`cb_columns` 2 vs 0, the search and
  their cloud together): squall line from 5 km 15.0 / 13.3 / 7.2 ms, towers 0.1-0.2 ms; MCS from 8 km 13.5-16.9 / 11.9-14.9
  / 5.7-9.4, towers 1.1-2.0 ms (~20%); hurricane eye at sea level looking at the eyewall 18.2-20.3 / 15.7-17.7 / 9.4-11.0,
  towers 4.5-6.0 ms (~55%); the same eye from 12 km 28-31 / 25-28.5 / 20-22.7, towers 16-19 ms (~82%: every sample of the
  eyewall's canopy walks a bin of up to `kCv2StormMaxPerBin` 12 towers). Levers measured there: light steps 6 -> 3 -0.8
  ms, `cdc_light` (S2, the user has it on) -1.6..-2.0 ms, `max_iters` 300 -> 200 nothing. CPU near the cyclone: the storm
  update 1.18 ms a frame with time running, ~20-27 ms once at a time jump (`weather.cpu_ms`); launch visuals ~1 ms there
  (plumes in view), of which the parcels' states 0.62 -> 0.36 ms with `Parcel::at` scanning from last frame's sample
  (`hint`) and rejecting an expired parcel from `ageEnd` (same answer as the binary search).

**Storm-view pass 2 (2026-10-24, issue 20261023_230322 "Storm performance": 12.4 km under an anvil at -0.26, 111.68, the
user's High settings, 1920x1009, RTX 3070 Ti; also 224032 from 3293 km, 223907 from 1.7 km, 230636 from 6.6 km; scripts,
variant generator and tables in `harness_runs/perf2/` — `mkv.py <parts>` builds text-substituted march variants from
`variants.py`, `gen_ab.py` interleaves them per view, `tab.py` tabulates; captures in `screenshots/overnight_20261023/perf2/`).**
Cloud march before -> after (interleaved in one app): 12.4 km 38.3-40.0 -> 34.6-35.1 ms; 6.6 km 14.0 -> 13.3; 1.7 km 11.8 ->
11.1-11.3; 3293 km 23.5 unchanged; Medium 12.4 km 21.1 -> 18.6. 128 registers (march and composite); captures within the
run-to-run floor (same shader reloaded twice: mean |diff| 0.7 at 12.4 km, 0.6 at 6.6 km).
- **What the 12.4-km view costs** (attribution by settings and cost probes): the eye is INSIDE the high layer, so every ray
  runs through its near haze in 60-120 m steps — `high_amount 0` takes the march 39 -> 16 ms; the thin-ice samples'
  lighting ~7 ms; the anvil shields' loop ~8 ms (`cv2ShieldAt`, ~13 candidates a lookup over the Maritime Continent's
  storm cluster, ~4 within reach; nearly every field evaluation does one) and the tower loop ~4 ms (both by running the loop
  twice, `x2shloop` / `x2towloop`); full-resolution clouds 24 ms (`full_res 0`: a look change). Sky pass ~12 ms: atmosphere
  loop ~6.5, god rays ~3, terrain ~0 (6 ms at the 1.7-km view).
- **Kept:** (1) the low field's EXACT top bound (`cv2FieldLow`, CPU `anvil3.zw`: the largest topMax over every weather-type
  mix at the tropopause factor's two ends; the bound is convex in the factor, so the shader interpolates) + the ground + the
  storm cumulus target only near a tower, then with its real value after the cluster read. The old test took the target's
  largest value, column.w x (1 + "Storm cumulus variation") = 21.6 km at the user's 12 km x 1.8, so every sample in the
  cirrus / anvil band paid the cell, smooth-weather and ground reads before topMax returned nothing (the old test is kept, so
  nothing it cut comes back). (2) `cv2StvT` from the finest level out, stopping at rem < 1e-5 (the same sum: it read all four
  levels, 32 flash-buffer reads, for every lit sample, thin ice included). (3) The tower search's height test before its bin
  lookup. (4) **"Near cirrus stride"** (opt-in, Rebuild experiments, slot 335, `clouds_v2.near_cirrus_stride`, 1 = off; UBO
  `cv2.perf` appended): a pure thin-ice coarse sample within a 12-m footprint stands for N steps: 12.4 km 34.2 -> 27.3 ms at 2,
  25.5 at 4 (Medium 18.6 -> 15.5 / 14.2), captures within the floor, still-view noise unchanged; off because a dense cloud
  starting inside a stride is entered up to ~2 steps late, and motion was not judged.
- **Tried and dropped (measured):** the clouds' shield index at 128 bins a face + a capsule filter (13 -> 11 candidates, no
  measurable gain, shield CPU 0.33 -> 1.5-1.8 ms a frame) or listed by discs along the spine (more entries); a bounding-sphere
  reject per shield (half the full tests, no gain); the shield loop two candidates at a time (+60%: the loop is neither ALU nor
  load-issue bound); lazy loads of tower / shield records; a strength-ordered break in the shield loop; the towers'
  trig / turret skips; height pre-tests before the high layer's weather reads and the anvil outline's reads; skipping the
  profile of towers far from their body (probe); `glslc -O` (+1 ms); the sky pass's god-ray froxels read a slice at a time
  along the ray (4 reads instead of 8 a lookup: no change). **Cost probes that duplicate a function call are superlinear**
  (a second `cv2ColumnSigma` +63 ms, a second `cv2FieldLow` +13 / +40 ms) while a duplicate in a branch never taken costs
  nothing: the executed code's size is part of the cost (Clouds v2 "Perf sprint": the instruction cache) — measure a loop by
  running it twice inside the function, not by calling the function twice.
- **Open:** the shields' and towers' loops (~12 ms here) want fewer candidates per sample, not cheaper ones (a 2D storm
  field cached per frame would be a look change); the near-cirrus haze (the option above); the thin-ice lighting (~5 ms);
  the sky's atmosphere loop (6.5 ms) and god rays (3 ms). CPU serial share ~2.5 ms a frame (record_compute 2.2: the dust
  list fill 0.8 incl. launch visuals 0.7, stars 0.5).
- Matrix suite (`--quick`, old shaders reloaded into the new build as the baseline: `harness_runs/perf2/mx_base` vs
  `mx_new`): every motion and noise cell identical to the printed precision; `compare` flagged alt3km's GPU (27.4 -> 31.3
  ms) while another agent's app shared the GPU, and the same cell interleaved in one app alone reads 9.9 -> 9.6 ms of
  cloud march (no regression). `hitch_check.py` fails A 1h / 1d / 1w (p99 51-54 ms) and D 1d / 1w (69-73) with the new
  shaders and A 1w, D 1d / 1w / 1yr with the OLD ones reloaded into the same build: the default views' GPU frame is now
  ~35-50 ms on this branch (storms), so the 50-ms p99 limits sit at the noise; the default view at five sim times,
  interleaved, is 0.5-0.9 ms cheaper with the new shaders (one 4.7-ms outlier did not repeat: 22.5 -> 21.6 on rerun).

**Perf pass 3 (2026-10-24, after the storm-look / perf 2 / wildfire merges; RTX 3070 Ti, 1920x1009, the matrix cells and the
user's report views 230322 / 223907 / 230636 / 224032 / 230542 at their own settings and at Medium; every number an
interleaved A/B in ONE live app (`shaders reload` variants or `set` variants, 3-4 reps), captures checked against the
same shader reloaded twice).**
- **The merged build's regressions.** (1) The SKY PASS +0.5..+0.9 ms in every cell was real and was the ring fix
  (450523b / b4a8687: the exact per-step density integral and the god rays' two profile weightings): old vs new sky SPVs
  in one app +0.55..+1.0 ms (the cloud SPVs swapped alone: 0). What it cost: three divisions and an extra vec3 exp a
  step, four exps a step for the god rays' weights, and a second inlined copy of the froxel / volume lookups (the
  piece-in-front loop). Recovered (5a012a8) without touching the result: one reciprocal for the three columns
  (`atmHalfAvg3(ea, eb, dh)`), the weights hoisted (ray constants), the review-22 dimming skipped while it is a no-op
  (uniform: god rays on or the eye's Sun clear), the step's height from the clamp's length and its end height from the
  ray's closed form, the Sun dot from the ENU direction, and ONE lookup loop for the step's reads and the piece's.
  SKY_TAA binary 820 -> 805 KB; sky pass vs the pre-merge shaders now +0.0..+0.4 ms (vs merged -0.3..-0.8); captures within
  the floor (ring view 230542 included). (2) The CLOUD MARCH near the ground +0.3..+0.6 ms: refuted. Old vs new march +
  lightning + composite SPVs interleaved: -0.2..+0.4 ms, within the noise (storm_gnd -0.2). (3) storm_gnd's still_grain
  5.5 -> 7.0: not a shader effect (old and new cloud shaders give the same image and grain in this build): the cell frames
  a different scene since the merges (the base run showed a heavy overhead deck, the merged build broken cumulus with the
  storms elsewhere in view; the storm catalog round the cell is the same).
- **Clouds resolve: the neighbourhood boxes once per 2x2 block** (d62c46f). On the sparse grid (every full-resolution
  cloud grid) the four pixels of a block share their sample b, and every box the resolve takes depends on b alone (the
  3x3 clamp box, the 7x7 still ring, the 5x5 motion colour box at the block's distance); each workgroup computes them
  per block in shared memory (the ring over 4 threads, reduced; the 3x3 by one thread in the old order) and the pixels
  read their block's. Resolve 1.52 -> 1.09 ms (High), 0.70 -> 0.51 (Medium). Tried first: the samples in a shared-memory
  tile (0 gain: the 7x7 ring was ALU, ~0.95 of the 1.5 ms by a probe that dropped it; the Catmull-Rom history ~0.1).
- **The density clipmap checks and bakes only the levels the march reads** (b9a6e71): cdc_max_level 6 (default and the
  user's), and levels 6-7 were never read (cv2_cdc.glsl returns unknown past it) but checked and baked every frame — they
  never stay fresh (at max level 8: 65536 cells baked and ~1M refused EVERY frame with time paused; not investigated why,
  since nothing reads them at 6). Settled, the gain is small (~0.03 ms of the 'layer_map' pass); it is the REFILL that
  shrinks: every `shaders reload`, `report load`, time jump or field change bumps the clipmap's epoch and all cells re-bake
  at 65536 a frame for ~75-100 frames (that pass 0.55-1.3 ms meanwhile), and with levels 6-7 out a refill is ~25% shorter
  and cheaper. (The first measurement compared refill frames against settled ones and claimed 0.55-1.3 -> 0.1 ms
  everywhere: wrong. Measure the clipmap only after ~150 frames of no reload / load.)
  (2026-10-26, the same trap again: eval counters taken ~100 frames after a jump read cdc_baked / cdc_refused 65536 /
  2.13M in every view, as if every in-shell cell went stale each frame. It is the fill: the motion guard keeps the clipmap
  off for 60 frames, then ~75 frames of 65536 bakes; by frame 250 both are 0, and from 6 km the coarse field evaluations
  fall 11.7M -> 2.7M a frame (harness_runs/ms/cnt2).)
- **No terrain sun-shadow march under the clouds' shadow** (cloudShadowT < 1e-3: every use is x cloudShadowT):
  the 1.7-km tornado view sky 12.85 -> 10.81 ms. The terrain shadow march is ~1.9-2.2 ms of a clear ground view
  (`terrain_shadow_strength` 0 A/B); cutting it to 10 steps / a 14% floor was -0.5..-0.75 ms with visible differences
  (not adopted, a candidate option).
- **CPU:** updateStars 0.5 -> 0.26 ms at dusk / night (pointPsf's per-star log10s of the optics gain hoisted: the sprite
  size from I^gamma x a frame constant; packed colours once). The serial CPU share of a frame is still ~1.8-2.3 ms
  (record_compute: dome / stars / ambience ~0.55, dust + launch visuals ~0.45, thunderStep ~0.24 in a storm).
- **Opt-in, measured:** "God ray reads (near, 8 = full)" (`clouds_v2.godray_reads`, slot 336, UBO `cv2.perf.z`): 4 =
  sky -0.5..-1.1 ms (R1 -1.1, R3 -0.65, ground -0.7, R1 at Medium -0.5), settled captures within the floor, more for the
  sky TAA to average in motion. "Near cirrus stride" (perf pass 2) stays the other one.
- **Tried and dropped (measured):** the god-ray froxel reads with the last two depth slices kept (0-8 loads instead of 8:
  slower, +0.0..+0.5 ms: the loop is not load-bound); the froxel fill skipping cells above the Sun volume (0); the sky
  glow bins under the glow's floor skipped (kept: exact, no measurable gain); the terrain march's tail and bisection
  through one evaluation site (-19 KB binary, timing mixed +-0.3); removing the knockout-only observer-height fallback
  (-34 KB binary, no gain: dead code does not cost, the code a step RUNS does); gating the terrain materials / shadow /
  city / sea reflection on the cloud composite's transmittance (< 1e-4: no gain); the camera transmittance carried along
  the ray at night (one exp a step instead of two: slower); the Mie / haze exps skipped high up (0); a 16x16 shared tile
  of the resolve's new samples (0).
- **Harness:** `shaders reload sky=<spv> sky_taa=<spv>` (docs/HARNESS.md): sky variants interleave in one app like the
  cloud passes'; `sky_stats` gives the TAA pipeline's register count and binary size (168 registers, ~805 KB).
- Tools (untracked): the scratch generators that built these batches (variant SPVs by text substitution under
  `build/Release/shaders/v/`, interleaved batches over mx cells and report views, per-pass tables) are described here
  only.
- **Matrix suite** (`--quick`, 1920x1009, `harness_runs/mx3/run` vs `harness_runs/mx_integ/merged` and `/base`): summed over the 21
  cells GPU / cloud bucket / sky pass base 508.1 / 243.1 / 210.0 ms, merged 515.4 / 236.9 / 223.8, perf 3 492.7 / 224.5 /
  213.4 (-22.7 ms, -4.4% vs merged; every cell 0.3-1.6 ms lower). `compare`: alt3km and alt1km walk worst frames
  64.7 -> 31.2 and 43 -> 25 ms; one flag, alt1km fast's worst frame 30.9 -> 74.1 ms (path mean 27.3 -> 29.1, a single
  spike in the sequence: the cell rerun alone three times, mean 25.9-26.1 ms, worst 29.2-29.4).
- **hitch_check** (`harness_runs/hitch_perf3`, alone on the GPU): fails at the fast rates as the earlier runs did (10-23:
  A 1w, D 1d / 1w; 10-24 03:00: A 1w, D 1d / 1w / 1yr), now A 1h / 1d / 1w (p99 54.6 / 57.6 / 57.3, limit 50), C 1d / 1w
  (53.3 / 54.6) and D 1d / 1w (76.4 / 71.9). The worst frames are `app.wait_fence` 43-54 ms, i.e. the GPU. Not this pass's
  shaders: at 1 h/s and 1 d/s the merged build's SPVs and these, interleaved 3x in one app, read the same (GPU 37.1 vs 38.8
  and 34.1 vs 32.3 ms, scene noise +-5 ms while time runs). What it is: at 1 h/s `march_a` is ~18 ms against 5.4 at 1x and
  at 5 min/s (the clipmap is off past 4x in both, so not that alone); not attributed. D's 1d worst frame also carries
  `kesslerEnsureCapacity` 11.8 ms (a pool growth on the main thread).

**Perf pass 4 (2026-10-25, after the wildfire / haboob / tornado reworks and the one-button Watch; RTX 3070 Ti; the Watch
landings with the user's settings at 1600x900 and at Medium, and the matrix cells at 1920x1009; A/Bs interleaved in one app
with `shaders reload` variants, `hr/` scripts in the perf4 worktree, untracked).**
- **The haboob's clear air was a veil.** x (1 - 0.95 z) left 5% of the shelf deck over the whole view ahead of the wall,
  a thin milky layer every ray marched and lit for tens of km: the haboob Watch's cloud march 20.3 ms, 12.5 with the clear
  air off, 13.0 with it clearing OUTRIGHT (`1 - smoothstep(0, 0.8, z)`), the wall's own cost ~0 (removing it made the view
  slower: the wall hides what is behind it). A look change, the intended one (the wall under open sky with the storm
  standing behind it; the veil hid both): `screenshots/rework_20261024/perf4/haboob_watch_before_veil.png` / `_after_clear`.
  Haboob Watch 37.2 -> 30.0 ms (user, interleaved), 17.2 -> 14.8 at Medium.
- **The haboob code cost every view.** The matrix against the pre-rework build (e1f3ee4, built separately) put the merges at
  +0.3..+0.9 ms of cloud march in every cell (summed GPU 492.5 -> 501.6 ms over 21 cells). Variants: the dust-wall lookup's
  code in cv2Field, never run (no wall in the grid), was it — 0.5-1.2 ms (binary 350.7 vs 341.9 KB); a uniform gate (UBO
  instead of the storage-buffer head) changed nothing: code size, not the loads. So the march is built twice (spec constant
  3, Haboobs above) and the dust-free pair is bound while no wall is near: cells -0.6..-1.2 ms, captures within the noise
  floor (mean |diff| 0.13-0.38), both at 128 registers. Matrix after: 494.2 ms summed (cloud bucket 223.1 base / 232.9
  merged / 224.6 now). The lightning pass's own copy of the code measured nothing (left as is). The matrix's storm_gnd flag
  (11.9 -> 14.0 ms, new high streaks in the capture) is sequence history: fresh launches of both builds draw the same streaks
  at 13.97 / 14.25 ms.
- **Smoke's Sun transmittance** (sat_sky.frag `smokeSunTransmit`, up to 96 Gaussians a ground pixel by day) is skipped
  under full cloud shadow: every use of `sunSpecTint` is x cloudShadowT (exact).
- **Measured, not changed:** after a Watch jump the first ~20 frames cost ~2x the settled cloud time (the clipmap and the
  Sun volume refilling: haboob 51 ms for ~0.3 s, tornado 33-37) — inherent to a teleport. The Watch views still over
  16.7 ms at Medium are terrain-bound and old: the tornado Watch (Nepal foothills, 18.8 ms at 1600x900): terrain detail
  ~6.6 ms and its erosion octaves ~2.8 ms (at 1920x1009; `terrain_detail_strength` / `terrain_erosion_strength` 0), terrain
  shadows ~1.1. Today's other additions are small: wildfires ~1 ms where a big fire is near (cloud 0.4, sky 0.6), the
  tornado composite ~0.1, launch plumes 1.5 ms at the crew Watch with the user's plume gain 0.3.
- Watch landings after (user settings / Medium, ms GPU): tornado 26.2 / 18.8, waterspout 19.1 / 12.7, haboob 26.1 / 14.8,
  supercell 17.3 / 13.7, squall line 19.1 / 14.1, wildfire 25.8 / 15.4, launch 12.6 / 7.5, twilight 16.2 / 10.1, crew 23.9 /
  14.2, the Sun's Fury 21.7 / 13.3, re-entry 13.2 / 7.1 (separate launches: +-0.4 run to run; the hurricane card found no
  storm at that moment).

**Weather perf pass (2026-10-27, on perf-frames; RTX 3070 Ti, the harness's 1600x900, default settings except the
Maritime view's (`report load` of issue 20261023_230322 = the user's High settings); every number an interleaved A/B in ONE
live app, 3-4 reps, captures against the same configuration reloaded twice).** Views: a hurricane from 12 km over its eye
and from sea level (`storms go cyclone=strongest dist_km=10 alt=12000|30`), a squall line from 6 km, a supercell from the
ground (`storms go kind=super dist_km=35 alt=300`), the Maritime Continent storm cluster, a haboob (`haboob`).
- **What the storm objects cost, measured first.** Loop-doubling probes (the tower loop / the shield loop run twice inside
  their functions: idempotent maxima) put the tower loop at ~0 in the hurricane, squall and supercell views, 0.6 ms in the
  haboob view, ~1 ms in the Maritime cluster, the shield loop at ~0 everywhere but the cluster (2.4-2.6 ms there). The bin
  lookups themselves (two atans + the hash probe for every field sample below the tropopause) are 0.07 ms in a calm view.
  What storms cost is mostly their CLOUD, not the search: `cb_columns 0` takes 6.7 ms off the 12-km eye view (the loop:
  0), `anvil 0` 3.3-4.0 ms (eye, cluster), `high_amount 0` 4-6 ms.
- **Kept: the tight index (`clouds_v2.storm_index_tight`, default on, exact).** `listStormBins` listed a tower (a shield)
  in every bin of the rectangle bounding its reach disc (its capsule's bounding disc): a sample walked every tower whose
  disc touched its BIN, ~2.2x (towers) / ~3.8x (shields, 156-km bins) the ones that can reach it. Now each row of the
  rectangle is cut, from both ends, to the bins that can hold a point within the reach (the shield's 1.6 radii along its
  spine, `KeepDisc` / `KeepCapsule`: from the bin's centre and its chord circumradius, `BinGeom`; the run between the ends
  is kept whole: a superset). The march's shield index is now 128 bins a face side (~78 km; the count rides in
  `cv2ShieldHead.y`, tables 32768 / 131072; the bake's envelope index keeps 64), and the per-frame listings run on the
  storm pool from 384 items. Maritime cluster, cloud march: 16.03 -> 14.91 ms (-1.1, every rep lower; tight alone at 64 /
  128 bins ~-0.3 each, 128 bins ~-0.4); the other views within noise; captures within the floor everywhere (and the new
  build's cluster capture matches the old build's at 0.55 levels vs a 0.58 floor). Entries (cluster): towers 13758 ->
  11714, shields 3578 -> 7459 in 4912 bins (from 2221). CPU: updateStorms ~+0.05-0.1 ms a frame (0.95-1.05 -> 1.0-1.15 at
  1x). `clouds_v2.storm_shield_bins_ab 64` (no UI, not for players) lists the old shield bins: the A/B.
- **Kept: two exact skips in the shield path** (128 registers): `cv2ShieldAt` breaks once the best shield found is at least
  as strong as the next listed (a bin lists them strongest first, and a shield's share never exceeds its strength), and
  `cv2AnvilSigmaP` returns before its three fetches where the best shield's distance is past 1.25 / 0.8 radii (the fray is
  floored at 0.8: the outline is 0 there whatever the noise) and no tower head is near. Cluster: -0.30 ms (the fetch skip
  -0.19, the break -0.05); the eye view within noise. **All together** (the old listing and bins vs the new, one app, the
  same layout): cluster 15.64 -> 14.83 and 16.03 -> 14.91 ms in two runs; a calm view at 3 km, orbit at 420 km and the
  ground: within noise, captures at the floor.
- **Kept: no surge test over the open sea** (terrain_detail.glsl `tdDemAt`, exact): `tdSurgeAt` ran on every height
  evaluation over a cyclone's sea; past 0.975 of the water map's reach (its rise tapers to 0 by 0.95, the shore offset is 0
  whatever the shift) it is skipped. Sky pass at sea level under a hurricane 7.27 -> 6.92 ms; at the landfall view (the
  coast, where it applies) unchanged; captures within the floor.
- **The weather bake at 1 h/s** (`state` -> `weather.cube.bake_ms_per_face`, `shaders reload weather=` variants): 1.35-1.4 ms
  a face near a cyclone or the cluster, ~3-4 faces a frame. Probes: the towers' / lines' stamp 0.25 ms a face, the
  cyclone's 3x3 supersample 0.12, the coarse levels' coverage fraction 0.15; the rest is the map reads. acos -> dot in
  `cycloneSwirl` / `cycloneNear`: -0.01 (noise, not kept). The tight envelope index changed nothing on the GPU (the bake's
  cost is its lookups and map reads, not its loops) and -0.1 ms of `env.towerIndex`.
- **Tried and dropped:** (1) a FAR-BODY SKIP in the tower loop (a tower whose body cannot come within a bound of the lobes'
  and the head lift's largest reach takes no profile; exact with a bound uniform across the sample's towers): +0.3-0.45 ms
  in the cluster, 0 elsewhere — the skip diverges within a group of 32 rays and adds code. (2) The tower bins' count read
  from the head (runtime n, `cv2StormBinKeyN`): +0.18 ms (n=3) in the cluster; the towers keep the constant 256. (3) Tower
  bins 512 (tight): within noise of 256, 3x the entries (CPU, and the list's capacity at 8192 towers). (4) Shield bins 256:
  the 16384 table overflowed (entries lost: a look change); with room, within noise of 128. (5) The first cut of the tight
  test read a bin's radius before setting it (`keep(binCentre(..., rad), rad)`: argument order unspecified) and dropped
  bins it needed; its -1.3 ms in the cluster was partly that.
- **CPU** (hitches bysum, 1x / 1 min/s / 1 h/s): the frames are GPU-bound (wait_fence 12-34 ms); weather on the main thread
  is updateStorms ~1 ms a frame (catalog 0.15, tower bins 0.15-0.2, index 0.25-0.3, shields 0.3-0.45), at warp + the
  envelope index 1.1-1.3 ms on frames the bake runs (every frame from ~1 min/s), the dust grid every 2 s on workers, the
  Weather window's Storms tab 0.15 ms of build_ui. A time jump costs one frame of updateStorms ~10-15 ms (the catalog's
  new bins, a new year's cyclone seasons ~7 ms: `wx.buildSeasons`). Not changed: rebuilding the bins only every ~20 s of sim time at low
  warp (keyed tower slots, ~0.6 ms a frame) — the frames are GPU-bound and new towers would wait for the next build.
- **What remains:** the storm CLOUD (the eyewall's towers ~6.7 ms from 12 km, the CDO / anvils 3-4 ms, the cirrus band the
  eye is inside), the cluster's shield loop (~1.8 ms by the probe at 128 bins: the group's longest bin, not the mean, sets
  it), the bake's map reads at warp (~4-5 ms a frame at 1 h/s).

**Weather perf pass 2 (2026-10-27, same machine and method; views: the hurricane eye from 12 km and from 30 m, and from
420 km, `time j2000 1164300000` then `storms go cyclone=strongest`; look checks = the same 40-frame 1 h/s run from one
moment, variants A B A B, with `clouds_v2.auto_exposure false`: the first run of a batch drifts from the rest).**
- **Kept: the visible-rectangle bake at warp** (`weatherViewRect`, `recordWeatherEvolution`; `clouds_v2.weather_rect_bake`
  true, `weather_rect_levels` 5, `weather_rect_max_share` 0.5, `weather_rect_margin_deg` 12, no UI). In time warp (the
  all-six path) a face in view bakes its levels of 128+ texels a side (0-4) only over the eye's cap projected onto the
  face: the eye's horizon + 12 deg (the cloud tops beyond the horizon, the flow's and evolution's displacements, the Sun
  volume's and the lightning lattice's reach), padded 16 + N/64 texels (coarser levels 2 texels: every level is read
  filtered); levels under 128 and a face whose rectangle is over half of it bake whole. A face's visible part is due by
  `max(cv2WxRectT, cv2WxBakedT)` when the new rectangle lies inside the last one, else by `cv2WxBakedT`; its remainder is
  due at the out-of-view rate and takes the frame's ONE full bake in turn with the faces out of view (the most behind).
  Outside time warp (slices) nothing changes. The shader takes a first column (`wx.w`, the PC's unused w) as it took a
  first row. Bake GPU time a frame (state `weather.cube.bake_ms`, 3 reps): 12-km eye 3.38 -> 1.70 ms, sea level 2.79 ->
  1.89, 420 km 5.78 -> 4.26; 5 min/s unchanged (~1.5, the quota bakes ~one face a frame either way). Look: A-B pairs 0.21 /
  0.27 / 0.62 levels against A-A 0.53 / 0.87 / 0.93 (eye, eye with levels 0-4, toward the limb from 420 km; heatmaps alike).
  `weather_rect_max_share` 0.9 was no better (orbit 3.60 -> 4.29 ms: more faces as rectangles crowd the one full bake).
- **Kept: the bake's envelope index off the main thread** (`clouds_v2.storm_env_async` true, no UI). After a frame that
  baked in time warp (`cv2WxLastAllSix`) and took the volumetric path (`stormsMainPathLast`), `updateStorms` runs at the top
  of `recordCompute`, right after the frame's sim time is final (it reads only that and the settings; recordCloudsV2 then
  skips its own call), and `buildStormEnvIndex` starts on threads of its own (`ownThreadEntries`: half the hardware
  threads, the same chunks joined in order; NOT the storm pool, which the main thread uses later in the frame:
  `Pool::run` is not reentrant), joined in `recordScreenshotCopy` before the frame's uploads (also at the top of
  updateStorms, before a new job, and in `destroyStormBuffer`); the mirror ranges are marked at the join. Same data in the
  same order. 12-km eye at 1 h/s: main-thread `record_compute` 5.26 / 5.43 -> 4.47 / 4.71 ms, the join waits 0.003 ms (since 2026-10-27 at most 1 ms, `kStormEnvJoinWaitMs`, a spin: hitch_check D saw 11.7 ms at 1 mo/s; a job still running then is joined by the next updateStorms, the frame's bake reads the previous index, and that next frame starts no job, `stormEnvHoldFrame`; `weather.cube.env_late`);
  the job itself 1.2-1.35 ms (state `weather.cube.env_job_ms`). Started where recordWeatherEvolution decides to bake (the
  first cut) it overlapped only the frame's last ~1.5 ms and the join waited ~1.1 ms (no gain).
- **Tried and dropped:** (1) `fillStormIndex` hashing each entry once and writing its GPU table slot by slot instead of a
  1 MB copy: env.towerIndex 0.60 -> 0.68 ms (noise; the cost is not the copy or the second hash). (2) Prewarming the next
  year's cyclone seasons on a worker: no effect measured — at the warps that cross a year in practice the storm update
  does no work (`updateStorms` 0.001 ms at 1 d/s), and the seasons a jump needs (~2.3 ms on the frame, with the
  catalog's new bins ~5.5) cannot be predicted. Note the year index is t / 365.2422 days from J2000: it turns on a
  different hour every year (2030 -> 2031 at 2030-12-31 ~12:12 UTC in sim time), and the default start already caches
  y +- 1.
- **Hitches** near the cyclone (12 km, threshold 50): 1x / 1 min/s / 1 h/s max 42 / 51 / 49 ms, every frame over 50 the
  GPU (`app.wait_fence` 45 ms); a 1 d jump and a jump to another year at 1x: max 51 ms.
- **Storm FPS (2026-10-27, issue 20261027_183909 "FPS near storms": 12.2 km at 20.9 N 98.3 E, the user's High settings, FOV
  120, 1920x1009).** Uncontended, still: GPU ~33 ms = cloud bucket 25.5 (march_a 22.6, resolve 1.1, composite 0.7, lightning
  0.5, froxels 0.5, sky_ap 0.4, clipmap 0.4) + sky 5.5 + scene depth 1.4 + orbit 0.6; CPU ~2.7 ms of record_compute (storms
  ~1). Settings A/Bs, interleaved, each after 170 frames (a field change refills the clipmap: ~40 frames after one read
  +10 ms): full-resolution clouds -12 ms, the high layer (the eye at 12.2 km is INSIDE the cirrus band, as perf pass 2)
  -10.7, anvil shields -6.8, storm objects -6, Cb columns -3.8, mid layer and light steps 3 ~0, near cirrus stride 1 +2.6.
  Flying the report's trail (~40 km/s at 8-12 km, `path play fps=60`): GPU 40.5 ms, cloud 31.5; `cdc_motion_guard 0` 25.0:
  the clipmap's motion guard (off past 0.4% of 20 km a frame) costs the ~6 ms of moving. Content, no bug found; the first
  path play after a load read 60-74 ms (spikes to 742 ms): refills, not steady state. Its scene: harness_runs/q1027 (untracked).
- **Remaining:** the bake's one full face a frame at warp (~1.35 ms: the faces out of view and the in-view faces'
  remainders, which nothing on screen reads), the eye view's storm cloud (content: the eyewall towers, CDO and cirrus), the
  warp's `march_a` without the clipmap (+2-4 ms at 5 min/s and 1 h/s vs 1x: the clipmap is off above `cdc_max_rate`, its
  cells would expire faster than the budget bakes them), the main thread's `updateStorms` ~1 ms a frame.

**Perf pass 5 (2026-10-29, branch perfhigh; RTX 3070 Ti, 1920x1009, the user's settings, every number an interleaved A/B
in ONE live app alone on the GPU (`SATLIGHT_HARNESS_MAX_APPS=1`), 3 reps; captures against the same configuration twice;
scripts `harness_runs/ph/` (untracked): `gen_ab.py` / `multi_ab.py` build batches, `abtab.py` tabulates, `mkv.py` builds
text-substituted SPV variants, `capdiff.py` the parity check).** Harness `shaders reload depth=<spv>` (new) swaps the scene
depth pass, so its variants interleave like the sky's and the clouds'.
- **Centimetre terrain detail skipped** (terrain_detail.glsl `tdAmp0`: x smoothstep(0.02, 0.04 m) of the octave-0
  amplitude). Low flat land (deltas, coastal plains: h0 a few metres from the DEM's bilinear filter) ran all eight octaves
  and the erosion at centimetre amplitude. Shanghai from 3 km sky pass 13.2 -> 9.6 ms.
- **The half-res terrain shadow on 0-m land** (scene_depth.comp): computed where the water map says land (the sky pass's
  terrain rule), not only where h0 > 0; land the DEM reads at 0 m fell to the sky pass's per-pixel march.
- **Water clearance in the terrain march** (`gTdWaterClearM`, set by tdDemAt over water: the water map's shore distance
  less 4.5 km, none near a coast patch, a pad or a storm surge): a step over water may cover it (at most 12 km). A grazing
  ray over the open sea took ~200 relaxation steps to its horizon. Sea from 20 m -1.8 ms, the storm from the sea -1.6.
  Together (depth + sky): Shanghai 3 km -4.1 ms GPU, 1 km -2.6, the ground -3.0, Geneva / Big Sur / the Cape within noise.
- **The atmosphere loop behind opaque cloud** (sat_sky.frag, full variants, band pixels only): where the cloud in front
  is opaque (transmittance < 1e-3) the loop ends just past the air's split (`tAirFrontM`), its steps over the air in front.
  Storm views -0.5..-0.6 ms.
- **Aurora culls** (cloud_march.comp): no march for a ray sunlit at every point (exact: the emission is x the sample's
  night, bounded between the 5 bound samples by half their arc), nor behind opaque cloud seen from below (x T^3). Storm at
  dusk -0.4 ms.
- **Moving costs the clipmap** (`cdc_motion_guard`, `cdc_motion_low_frac` 0.25, `_epoch` 4, `_max_m` 30000; 2026-11-04,
  qa clip-motion): inside the shell (eye below the shell top + 2 km, any height) the clipmap stays on while the eye moves
  up to 25% of the guard's 20-km scale a frame (5 km: 9 km/s at ~2 fps), with the bake's footprint bracket (clouds.md
  "Low in the shell"); above that the old guard (0.4% a frame) still drops it. What made the kept-on clipmap lose cloud
  masses at 12 km over storm tops was NOT the bake's footprint seen from another eye: it was the low shape volume's frame
  (`cv2ShapeFrame`, one of four tile rotations chosen at the observer), missing from the clipmap's epoch. Crossing a line
  where the choice changes reshapes every lobe; cells baked under the old frame read empty where the new lobes are.
  The user's storm trail crosses one ~1.3 km before its t = 6 s pose. Found with the miss counters
  (`-DCV2_EVAL_COUNT`, `cdc_missed`): flying 20 km into that pose at 150 m a frame left 30-37k missed skips a frame
  (level 0), the same after a 1.5-km TELEPORT and not after a 1-km one, at a fixed 300-m footprint in bake and check
  alike; `shape_frame` (harness `state`) switched 0 -> 2 between 1.25 and 1.5 km. With the frame in the epoch: ~700
  a frame, the settled view's level. Dead ends on the way (each made no difference before the fix): 3 bracket reads
  with a height margin, the stretch paired with the footprint, 4 corner reads of the (distance, height) box, an
  eye-independent S2 footprint, a 2x wider sample box.
  Quality (tstab-style, motion frames vs settled references, % of pixels > 16 levels / mean |diff|, 60 fps; frames of
  `harness_runs/cm/gen.py`, scored by `score.py`): storm trail 1.36% / 2.01 (guard) -> 0.21% / 0.78 (kept on), anvil
  walk 5.9% / 4.2 -> 3.3% / 3.4, cumulus walk at ~2 km 0.24% / 1.51 -> 0.15% / 1.51 -- the kept-on frames are CLOSER to
  the settled ones, because a settled view has the clipmap's S2 light and the guard's moving frames had not. At 10 fps
  (1 km a frame at the walk): anvil 4.4% -> 3.2%, storm trail 2.3% -> 1.4% at 25%; 5% a frame dropped the clipmap there.
  Cost (cloud bucket, RTX 3070 Ti, 1600x900, High, full res, min of the uncontended interleaved reps; another app shared
  the GPU for the rest): anvil walk (issue 20261104_111947, 11 km, 9 km/s) still 49 / 49 ms, moving 75 -> 67; storm
  trail (12 km) still 37 / 37, moving 54 -> 45. Moving then still cost 1.2-1.4x standing still: the march's ray count
  (360k rays a frame moving vs 216k still at the anvil, `march_rays`), not the clipmap (it skipped as many coarse steps
  moving as still, misses 0.011%) -- the smooth tiles, next item. (The earlier "121 ms walking" at the anvil was with
  the GPU shared.)
- **Moving marched every sparse slot: the smooth tiles were off** (`smooth_moving` 0.5; 2026-11-04, qa motion-rays,
  `harness_runs/mr/`). The extra rays in motion are not disocclusion, a repair pass or the resolve: the full-res grid
  marches 1 slot per 2x2 a frame moving or still (`sparse_block` 2, no moving mode at 11 km: `full_rate_above_km` 30),
  and a still view marches FEWER, because its smooth tiles (cloud_v2_tiles.comp kMode 1, threshold 30 levels) take a
  quarter of that, and the smooth tiles needed 64 eye-still frames and a step <= 150 m (issue 20261027_004019). Counters
  (`-DCV2_EVAL_COUNT`, path stepped a frame at a time, 1600x900, rays / coarse + fine evaluations): anvil still 224k /
  9.1M, moving 360k / 13.7M, smooth tiles off still 360k; storm trail (183909, 12 km) 158k / 2.9M still, 360k / 8.0M
  moving; cumulus walk (~2 km) 93k still, 162k moving. With the tiles kept on in motion at the full threshold the moving
  frame marches the still frame's rays (anvil 220k, storm 149k, cumulus 98k); at half the threshold (the default, 15
  levels while the eye moves, 30 once it has been still 64 frames) 260k / 180k. Per pass (eased timers, anvil): the
  march 38 still, 47-49 moving -> 39-41; what is left of moving's extra is the clipmap's bake following the eye
  (`layer_map` 0.1 -> 3.4 ms) and 1-3 ms of march.
  Cost (cloud bucket, min of 2-3 interleaved reps, RTX 3070 Ti, 1600x900, High; a second app shared the GPU in most
  reps, so read the ratios): 60 fps, off / 0.5 / 1: anvil walk 71.0 / 65.0 / 61.4 (still 46-47), storm trail 36.0 / 32.5 /
  30.1 (still 29); anvil trail 33.9 / 32.6 / 29.9 (still 13.5-14). Uncontended, off / 1: anvil walk 38.5 / 35.2 (still
  27-29), storm 15.7 / 12.3 (still 12-14), cumulus 4.7 / 4.6. 10 fps (1 km a frame): anvil walk 77.4 / 72.3 / 68.6,
  anvil trail 83.7 / 73.7 / 64.8, storm 45.9 / 38.3 / 35.9 (uncontended off / 1: 44.9 / 40.2, 45.9 / 36.4, 22.5 / 17.5).
  Quality (gen.py + score.py frames vs settled refs, % px > 16 levels, mean |diff|; A-vs-A spread 0.01-0.11% / 0.01-0.04):
  off / 0.5 / 1 at 10 fps: anvil walk 3.81 / 3.90 / 4.05% (mean 3.42 / 3.43 / 3.44), anvil trail 13.2 / 13.1 / 13.2,
  storm 0.99 / 1.00 / 0.97, cumulus 0.82 / - / 0.81; at 60 fps: anvil walk 3.23 / 3.19 / 3.05, anvil trail 4.24 /
  4.33 / 4.44, storm 0.17 / - / 0.17, cumulus 0.17 / - / 0.15. At 1 the 10-fps anvil walk is +6% (consistent over 3
  batches) with slightly coarser stair-steps on cloud edges (the tiles' 4x4 lattice under the tent): 0.5 sits at the
  A-vs-A spread for about half the saving. The 004019 squares after a fast flight do not return (that trail's end and
  the 100 frames after it: 0.12-0.14 levels from settled, 0.17-0.20 with the tiles off).
- **Rays per frame** (2026-11-04, clouds.md "Amortization"): the full-res grid marches 1 pixel in 4 a frame; at the
  anvil the march is proportional to its rays (`sparse_block` 2 / 3 / 4: 43 / 29 / 21-24 ms still, bucket 121 / 68 / 47
  walking), storm 10.7 / 9.8 / 6.5 (walking 26 / 16.5 / 13.5), cumulus 3.0 / 2.0 / 1.6. 3 / 4 cost ~5-10% / 15-25% more
  motion error (tstab-style vs settled) at 60 fps: not adopted as the default.
- **Layer mask in the occupancy clipmap** (2026-11-04, clouds.md occupancy clipmap > "Layer mask"; proposal 1 of the
  storm-perf write-up): `cv2Field` calls only the layer functions the point's clipmap cell can need. Cloud march
  (RTX 3070 Ti, 1600x900, storm report's settings, perf-frames 4f97691 -> this, interleaved x3): storm shot 1 still 14.2
  -> 12.6 ms, flying east from it at 4.9 km/s 21.4 -> 19.1, shot 2 7.5 -> 7.2, cumulus from 3 km 9.3 -> 8.4; orbit 6.6
  -> 6.8 and the report's two fast trails +0.4 / +0.1 (code size; gating the lookups by altitude changed nothing).
  128 registers, images at the floor. Estimated -2..-4 at shot 1: the towers' strength keeps their walk on in an As
  sheet within their reach. Next: the Sun-aligned light volume (proposal 2).
- **Storms through the cumulus field** (2026-11-04, issue 20261104_111947 "Anvil performance", clouds.md "Storms through
  the cumulus field"): at 11 km inside an anvil band 36 of ~38 shaded samples a ray were the shield's translucent haze,
  each a fine step and a full field read (shields knocked out: 74 -> 31 ms of cloud march). The shield's plate (presence >
  0.5) and the towers are now the low field's margin, one set of fetches; the haze beyond is thin ice at coarse steps.
  Cloud march (RTX 3070 Ti, 1920x1009, user settings, interleaved x3): 49.0 -> 28.9 ms still, 57.8 -> 36.7 on the report's
  trail; a plate seen from 19 km above +3 ms (a dense billowed lid where the old one was haze). The switch is a
  specialization constant: behind a uniform both paths cost ~0.6 ms of code size in every view.
- **A sunset anvil deck overhead** (2026-11-08, issue_20261108_172846 "Big laggy formation": 31.7 S 147.3 E, 2.7 / 4.9 km,
  FOV 120, the user's settings; an MCS storm object's shield plate, strength 0.8, lit from below by the setting Sun and by
  Reflect beams; evidence and batches `qa/waves/20261108/evidence/laggy/`). Cloud march 31-33 ms (GPU 42-45), frames
  GPU-bound (`app.wait_fence` 41-42 of 43.5-45.4 ms). Where it goes (RTX 3070 Ti, 1920x1009, interleaved in one app):
  storm objects off -16 ms (14.7 / 12.6 left; `cb_columns` 0 the same, it gates the storm geometry), `anvil` 0 -11.7 / -6,
  mid 0 -6 / -4.5, high 0 ~-1 / -2.7. The light march is half (probe off: -14.4 / -16.8): Reflect beams' gather and
  three occlusion reads -4.6 / -2.5 (visible: 9.5% of shot 1's pixels move > 8 levels without them), the far side's two
  reads -2.1, the sky probe -1.1 / -1.4, the far sample -1.0 / -1.5, the steps the rest. Rays: 396k a frame at shot 1 =
  82% of the 2x2 slots (the textured deck leaves few smooth tiles), 205k with the anvil off; per ray 12.3 coarse + 5.3
  fine reads, 2.8 lit samples (6.4 light reads each), 2.2 faint, 1.0 thin ice. The storm SEARCH is small: loop-doubling
  probes (x5) tower +0.8 / +2.6 ms, shield +1.3 / +1.3 (spread 1-2 ms), so a cached per-frame 2D storm field could save
  at most ~2-4 ms here (~10%; the plate margin and the towers' radius profile are per height, so per-sample maths stays).
  Empty air costs nothing (coarse skips at twice the stride: +0.2 / -1.3, noise). CPU: `updateStorms` 0.64 ms a frame at
  1x and nothing paused (`weather.cpu_ms` 0.55-0.62 at 1x: catalog 0.12-0.15, shields 0.12, tower index 0.30-0.34; a paused
  `state` shows the last update's 50 ms, the catalog rebuild of the report's time jump); the clipmap's bake follows the
  time quantum at 1x (`layer_map` 0.07 -> 1.6-2.0 ms). Kept: `sparse_block_still` (clouds.md, Amortization): 31.4 ->
  21.4 / 30.2 -> 23.7 ms still. Tried and dropped: skipping the light reads' field call where the clipmap says the cell is
  empty (exact; +1.0 / -3.1 +-3.3 ms, noise: the empty reads were already cheap), `light_steps` 5 (+1.7 / -1.0). The
  reported 362-ms frames were the HQ photo's (the user log: "Photo target 3840x2018" 12 s before the shot; its 48
  offscreen frames at 4x the pixels ran 185-370 ms): the report's `frame_ms` no longer counts them (capture.md).
- **Dead ends:** skipping the sky pass's terrain march where the four cloud texels are opaque (-0.1..-0.3 ms, changes the
  depth the sky TAA reprojects with); the history copies (~0.1 ms); FIFO quantisation as the cause of "30 / 20 / 15 FPS"
  (the reports' frame times are not quantised: 27 ms median at a 28 ms GPU).
- **Tried and dropped (2026-11-04): the light march's far steps from the Sun transmittance volume** (clouds.md, the S3
  section, "Not a light volume for the march"): -1.1 / -1.4 / -2.1 ms of cloud march at the cumulus / storm / anvil views
  (contended GPU, min of 3 reps), not the -4..-6 ms estimated: light steps 2-5 are S2 clipmap reads already, so only the
  far sample's field read goes. The image moved +6 levels at the sunset anvil (the volume's height knots span ~10 km
  along a ray under a 2-deg Sun).
- **Clouds and sky pass, 2026-11-09/10 (issue_20261109_184851; branch perf/20261109/cloudsky; evidence
  `harness_runs/cloudsky/`).** RTX 3070 Ti, 1920x1009, the user's settings.json. Kept, all ON with an off switch each
  (clouds.md): lit samples first, the faint stride (`CV2_FAINT_STRIDE`), the far-layer threshold (`far_layer_min_blend`),
  the orbit block (`sparse_block_orbit`, "Amortization"), the limb volume (`sky_ap_limb`, render-pipeline.md "THE LIMB
  VOLUME"). Tool: `tools/harness/cloudsky_matrix.py` (docs/HARNESS.md "Clouds-and-sky matrix").
  - **The matrix** (57 cells: 9 sites x 6 altitudes (2 m, 1.5 km, 6.5 km, 11 km, 300 km, 850 km) + night_3km + 2 Reflect-beam
    cells; stills, 16 noise frames, 16 live frames, 69 motion paths scored against settled references, time warp at
    1 min/s, 4 jumps, the report's 5 shots; sites: clear Sahara, broken cumulus Indian Ocean, dense cumulus Pacific 7 N,
    overcast Antarctic plateau, two live storm objects from 40 km, terminator Siberia, sunset into the Sun, night).
    GPU frame p50, ms, base -> after (mean of the cells; the worst cell after):

    | altitude | mean | worst after |
    |---|---|---|
    | 2 m | 20.4 -> 20.3 | term_2m 28.6 |
    | 1.5 km | 18.8 -> 18.5 | term_1km 22.1 |
    | 6.5 km | 20.6 -> 18.9 | term_6km 22.8 |
    | 11 km | 20.9 -> 19.1 | term_11km 21.1 |
    | 300 km | 30.2 -> 21.7 | dense_300km 25.0 |
    | 850 km | 39.5 -> 26.5 | ovc_850km 31.0 |

    | weather / light | mean | worst after |
    |---|---|---|
    | clear | 19.4 -> 18.0 | clear_850km 23.5 |
    | broken | 21.9 -> 19.4 | brk_850km 25.1 |
    | dense | 26.3 -> 21.5 | dense_850km 29.5 |
    | overcast | 31.5 -> 24.5 | ovc_850km 31.0 |
    | storm A | 28.0 -> 21.5 | stA_850km 29.0 |
    | storm B | 19.7 -> 18.1 | stB_850km 22.0 |
    | terminator | 29.9 -> 24.2 | term_2m 28.6 |
    | sunset | 25.3 -> 21.3 | sunset_850km 27.6 |
    | night | 23.5 -> 19.1 | night_850km 24.2 |
    | beams | 28.9 / 26.0 -> 30.4 / 28.0 | beams_site 30.4 |

  - Still cells over 25 ms: 23 -> 10 of 57. Motion paths over 25 ms mean: 39 -> 21 of 69. Per pass, mean over the 54 site
    stills: cloud bucket 14.8 -> 11.9 ms, sky pass 8.1 -> 6.8 ms; at 850 km cloud 28.9 -> 17.9, sky 8.4 -> 6.5; at 300 km
    20.3 -> 13.5, 7.7 -> 6.0; at 2 m 9.1 -> 9.3, 8.5 -> 8.1.
  - **The report's shots**, paused / running (p50): s1 70.5 -> 38.3 / 70.5 -> 39.1, s2 45.9 -> 29.7 / 47.0 -> 30.6, s3
    60.9 -> 35.5 / 62.0 -> 36.6, s4 62.9 -> 34.7 / 63.3 -> 35.9, s5 39.5 -> 29.2 / 39.6 -> 29.9. Their trails replayed (GPU
    mean): 60.9 -> 43.1, 44.4 -> 33.7, 61.3 -> 35.2, 52.7 -> 38.4, 44.4 -> 37.2. Paired, interleaved x3 in one app (best
    rep p50): s1 54.6 -> 36.3, s1 at 1 min/s 69.2 -> 36.7, s2 44.7 -> 28.8, s3 49.6 -> 28.9, s4 50.3 -> 29.3, s5 35.3 ->
    27.2, ovc_850km 42.3 -> 30.9, stA_850km 39.8 -> 27.7, dense_300km 29.9 -> 24.2, ovc_6km 25.2 -> 20.8, term_6km 25.1 ->
    21.8, dense_2m 16.5 -> 16.9.
  - **Moving**, worst after (GPU mean / max): stA_850km-fast 37.0 / 45.8 (base 42.9), brk_850km-fast 36.3 / 43.9 (43.1),
    ovc_6km-fast 33.8 / 42.9 (41.1), term_6km-fast 32.2 / 36.2 (37.0), beams_site-look 30.9 (27.7), stA_300km-fast 30.7
    (34.8). **Time warp 1 min/s** (p50): brk_300km 26.9 -> 20.8, dense_300km 35.4 -> 24.3, stA_300km 39.4 -> 25.0; at
    6.5 km 24.3 -> 23.8, 32.0 -> 30.2, 33.6 -> 32.9. **Jumps** (GPU mean, first 30 frames / next 60): to stA_2m 29.9 ->
    29.6 / 26.2 -> 25.9; to dense_6km 33.1 -> 31.2 / 28.6 -> 26.8; to ovc_850km 56.0 -> 33.6 / 54.9 -> 32.1 (first cut,
    block on after 8 frames); to brk_2m 24.4 -> 24.6.
  - **Stability:** noise (16 paused frames) and live (16 frames at 1x) unchanged in every cell (worst noise stA_1km 1.02 ->
    1.02; worst live brk_2m 9.79 -> 9.80). Motion: below 300 km every path within 0.15 levels of base. Orbit: clouds.md
    "THE ORBIT BLOCK". Stills base vs after (held exposure identical in every cell): mean |diff| under 1.5 levels except
    storm B at 1.5 km (4.8: the eye is inside the storm cloud; the same view captured five times in one app spans 115-135
    mean, whatever the configuration) and storm B at 2 m (2.8).
  - **Still above 25 ms, and what would get them there.** 850 km with dense / overcast / storm fields, still: 27-31 ms (was
    46-54); the march is ~18 ms at 1 ray in 9, and a 4x4 block there is ~-4 ms more (cloud bucket 23.7 vs 26.8 ms at shot
    1) but softer: not adopted. Fast (boosted) flight at 850 km: 36-37 ms (was 43); the block is off in motion (the image
    moves tens of pixels a frame), so it needs the resolve to trust depth-consistent history in fast motion
    (`history_depth_test`, off) or a cheaper ray. Fast flight over a deck at 6.5 km and at the terminator: 32-34 ms (was
    37-41): the 2x2 grid's ray count in motion. Terminator and overcast at 2 m (28 ms) and the Reflect-beam cells (28-30
    ms): the sky pass is 10-14 ms there (terrain, sea and the beams' own passes), the clouds 6-12 ms; untouched by this
    pass. The report's shots 1, 3, 4 (dense storm fields from orbit at sunset): 35-38 ms in the matrix run, 29-36 paired
    (was 61-70).
  - **Measurement traps.** SETTINGS: run.py / live.py without `--settings` start from the built-in defaults, an older
    cloud look (98 clouds_v2 keys differ by > 2%); every benchmark runs on the user's file. EXPOSURE: the suites set
    `clouds_v2.auto_exposure 0`, which clips every daytime frame; use `exposure fast` (settles the metered offset by a
    fixed share per frame), wait 45, `exposure hold` (holds it on a quarter-stop grid).
  - **Two speed regimes.** With identical work (rays, every evaluation counter, the clipmap's skip count, every exported
    state) the cloud bucket runs ~20-25% slower for tens of seconds after a `report load` (up to ~900 frames), ~200-350
    frames after a `shaders reload` or a cloud setting change, and on and off with time running. Only the cloud passes
    move (march -24%, composite -17%, resolve -9%); the sky pass does not. GPU clocks (1905 / 9501 MHz) and temperature
    are constant, no throttle flag; GPU power is LOWER in the slow phase (~210 W vs ~222 W). Cause not found. A 60-frame
    window is usually wholly in one regime. Protocol: wait >= 350 frames after any switch, >= 3 interleaved reps, report
    per-rep `gpu_total_ms` min / p50 and compare best reps; pair before / after in ONE app. An earlier "texture reads are
    half the march" compared across regimes (clouds.md, "Tried and dropped, 2026-11-09").
  - **The unstable in-cloud cell:** stB_1km (the eye inside a storm cloud) is not a stable still: five captures of one
    view span 115-135 mean levels, so it cannot gate an image comparison.
  - **Round 2 (2026-11-10, evidence `harness_runs/cloudsky/attr1`, `lc1`-`lc3`, `fog`, `reg`, `FINDINGS.md` section 7).**
    WHERE A GROUND FRAME GOES (1920x1009, the user's settings, knockouts twice a cell): the terrain march is the largest
    item, above the clouds -- `terrain_march` off = -8.8 ms at clear_2m (frame 20.7: cloud bucket 5.9, sky pass 9.7, scene
    depth 4.3), -8.4 ovc_2m, -7.1 term_2m, -12.1 at the beam site (28.8), and nearly all of it is the procedural detail
    (`clouds.terrain_detail_strength 0`: -7.7 / -7.3 / -9.9 ms; the sky pass 9.6 -> 5.1, scene depth 4.4 -> 1.0 at
    clear_2m), evaluated in three marches a frame (quarter, half, full resolution). `terrain_shadow_strength 0` -1..-1.5.
    Render scale 0.8 / 0.67 (sky TAA upscaling): -3.5 / -4.6 ms clear_2m, -4.5 / -7.9 beam site, all in the sky pass (scene
    depth and the cloud bucket do not follow it). From 850 km: fog_layer -2.3..-3.2 ms, atmosphere_loop -1.4..-1.7. The 8
    GPU buckets sum to the total within 0.15 ms.
    KEPT: no sky probe under a large pixel (cloud_v2_march.comp `CV2_PROBE_FP` 250; clouds.md, the perf pass's bullets).
    TRIED AND DROPPED: (1) a LIGHT CACHE -- a hash table (2^23 entries) of the light march's answers by world cell (the
    drifted cube-face coordinates + height; 2-4 footprints across, 16-128 m tall), read and written at a jittered
    position, 128 registers. One answer a cell: cloud bucket 27.1 -> 24.0 at shot 1 with grain on the tops (2.4 levels, 9%
    of pixels past 8); a running mean answering after 4-8 marches: 16.9 -> 14.0 (1600x900) with the tops mottled and grey
    (2.9 levels, 12%; floor 0.8, 0.3%); cells 4 footprints x 128 m: 24.2 -> 16.7, flat grey (5.2 levels, 28%). The light
    under a top changes over tens of metres of depth, so cells that keep the look are millions a view, each visited every
    few hundred frames, and a 32-lane group waits for any lane that marches. (2) The fog march at 8 / 6 steps above
    120 km: -0.2 / -0.4 ms. (3) The blue-noise tile in device-local memory (it is in plain host memory): no change.
    THE SPEED REGIMES, MORE (80 x `perf frames=45` at shot 1, four runs): the switch is abrupt (within one 45-frame
    sample): slow for 15-50 s after the load, then fast with relapses of ~2 s at irregular gaps. Slow / fast GPU 37.3 /
    31.2 ms (1920x1009); cloud bucket +30%, sky pass +19%, satellite_star_draw +18%, orbit_compute 0: the texture-heavy
    passes, so NOT the clouds alone as written above. GPU clock 1905 MHz and no clock-event flag throughout one run (4-5
    Hz log; another run showed the driver's gpu_idle flag at 1800 MHz for 24 s then 2-4 s bursts, the same pattern), power
    220 W slow / 231 W fast. Ruled out: other processes (Windows GPU Engine counters: only the app above 0.5%), every
    numeric leaf of `perf`'s state and CPU timings (a diff separates only the pass timers), uploads (1.56 MB, 27 copies a
    frame in both), the app's dedicated / shared GPU memory (2258 / 221 MB, constant), the app log. One submit a frame.
    Untested: the driver's power management mode (NVIDIA Control Panel, "Prefer maximum performance").
  - **Round 3 (2026-11-10): the terrain height clipmap** (terrain.md "The terrain height clipmap"): the value octaves of
    the terrain detail baked into 11 nested levels round the observer, read by the three terrain marches; the erosion
    still evaluated once per march. Ground frames -5 to -7.5 ms (clear_2m 21.3 -> 16.3, ovc_2m 27.1 -> 21.4, term_2m
    27.0 -> 21.8, beams_site 29.2 -> 23.5, the Alps from the ground 28.8 -> 21.4), nothing from 6 km up. Tried on the
    way: baking the erosion too (-10 to -12 ms, but soft gullies and flat low-Sun shading: not kept); the composite's
    (cloud_march.comp) per-pixel observerEffHeightDetailed replaced by the DEM alone: no change, left.
    RENDER SCALE on High, measured for the user's decision (harness_runs/cloudsky/r3): 80% = clear_2m 20.7 -> 17.9,
    term_2m 24.6 -> 21.4, beams_site 28.6 -> 23.4, stA_2m 18.9 -> 16.5, dense_6km 19.9 -> 16.5 (the sky pass only).
    `clouds_v2.sparse_block_orbit_moving` 1 (default 0, the user's decision from clips): the orbit block through a
    pan, 27.2 -> 20.3 ms (stA_850km), 28.9 -> 21.3 (brk_850km), 22.3 -> 19.6 (dense_300km); boosted flight unchanged
    (36.9 -> 33.8, 35.8 -> 36.3): the matrix's boosted path moves ~60 km a frame, past the 20-km step that drops the
    history (fillCloudsV2Params nearPrev), so every frame is a fresh 2x2 grid with no history, and the block's gate
    (history valid) never opens. `clouds_v2.history_jump_frac` (default 0 = 20 km at any height; EXPERIMENT, the
    user's decision from a clip): a step is a jump only past this share of the eye's height. At 0.1 the history
    survives the flight: stA_850km boosted 36.3 -> 35.7 ms alone, 26.6 ms with the moving block (cloud bucket 27.8 ->
    18.0), and the clouds smear along the flight direction where today they are crisp and grainy.


**The hurricane march pass (2026-11-11, hurrperf; issue_20261111_164105 "Hurricane performance", the eye from 24 km; RTX 3070
Ti, 1600x900, the reporter's settings, variants interleaved in one live app, 3 x `perf frames=60`; batches and variant
generators in `qa/waves/20261111/evidence/hurrperf/`):** (1) **Where it went.** The reporter's 74 ms was the march with the
occupancy clipmap OFF: their trail ran the clock at ~1 min/s (274 s of sim time in 12 s) and flew at up to 15-39 km/s, and
either switches the clipmap off (`cdc_max_rate`, the motion guard). The trail replayed at 1 min/s: cloud bucket 63-65 ms.
Still at shot 1 with the clipmap on, 46-67 ms between sessions (+-0.2-5 within one); march_a ~36, the clipmap's check +
bake ("layer_map") ~4.7 ms even still and paused. Eval counters a ray (`-DCV2_EVAL_COUNT`): 4.3 coarse, 5.0 fine, 9 clipmap
skips, 3.6 lit samples at 8.2 field reads each: the light march is ~80% of the field reads, and 29% of the lit samples lie
where the ray's transmittance is already < 0.1. Knockouts (cloud bucket vs the base, same session): the hurricane fill out
-3.5 ms, the ice table out -2.9, the flow map out +9 (slower), all three out ~0 (still 255 registers on this tree); the ray's
cut at T < 0.05 instead of 0.01 -6.8 ms. Light steps taken after the light's optical depth passes 30 / 80: 6% / < 1% (an
od cutoff in the light march buys nothing). The clipmap skipped ~93k steps a frame that held cloud (80k of them dense,
od > 0.1 over the step: blocks' area, the 2026-11-11 wave). (2) **Kept: THE REGISTER GUARD** (`CV2_REG_GUARD`,
cloud_v2_march.comp): two branches on a uniform that never holds (`cv2.misc.x < -1`) -- an atomic on the eval counters at
the light march's head, and a third `cv2Field` call site at the clipmap skip with an atomic on its result, the flash buffer
writable -- put the merged march in the 128-register regime (255 without; binary 549 -> ~696 KB, the never-run copy).
Either one alone, or the buffer readonly, leaves 255; the full `-DCV2_EVAL_COUNT` build was the first 128 seen. Behaviour
identical (image at the A-vs-A floor). FRAGILE: the regime follows code shape; check the Register Count after every edit.
(3) **Kept: DEEP LIGHT** (`clouds_v2.deep_light_t` 0.1, UBO `cv2.hurrperf.x`): a lit sample deeper than T 0.1 in its cloud
borrows the previous lit sample's optical depth toward the light + its own extinction over the extra depth; thresholds
0.2 / 0.3 were no faster. (4) **Measured, OFF: THE OPACITY STOP** (`clouds_v2.opacity_stop_t`, 0 = off; 0.05 tried): the
ray ends at that transmittance and its last sample takes the rest of the weight (the mean colour as the fill read greyer
over the CDO top). (5) **Numbers** (base / guard + deep light / + stop 0.05, cloud bucket ms): shot 1 still 67.4 (63.0
again) / 51.9 / 48.1; the trail at 1 min/s 62.8 (65.4 again) / 48.2 / 44.8, GPU p90 107.5 (112.7) / 81.3 / 71.2; the eye
from 45 km (issue_20261111_165853) 54.0 (56.6) / 47.1 / 43.8; under a hurricane (165715) 40.1 (42.7) / 38.0 / 35.1;
orbit 400 km 19.41 / 18.34; cumulus 1.5 km 10.55 / 10.34. Parity, mean |diff| vs the base (floor = the base twice): shot 1
2.26 (2.17), stop 2.54 with spots on the cloud edges; 45 km 1.16 (1.05), stop 1.00; under 6.36 (7.12), stop 7.37.
Tried and dropped: the fill / mass loops out of the light march's cv2Field copy only (255), one cyclone instead of the loop
(255), deep light with the probe's value kept as well (255).

---
