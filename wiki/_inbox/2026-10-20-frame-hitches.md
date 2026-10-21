---
pages: [development/profiling.md, development/harness.md]
code: [src/FrameProfile.h, src/App.cpp, src/simulations/SatelliteSimLaunches.cpp, src/simulations/SatelliteSimKessler.cpp, src/simulations/SatelliteSimHarness.cpp, tools/harness/scripts/hitch_ladder.satcmd]
---
## Now
The frame profile (`src/FrameProfile.h`) times named sections of the main thread's frame (`FPROF_SCOPE("name")`,
inclusive, other threads ignored); a frame over 50 ms (env `SATLIGHTSIM_HITCH_MS`, 0 = off) is logged to
satlight_log.txt as `hitch: wall N ms ...: <largest sections>`, with `app.wait_fence` = waiting for the GPU and
`cpu.*` the CPU buckets. Harness `hitches [clear|threshold <ms>]` gives the wall-time distribution (p50 / p99 / max,
frames over 50 / 100 / 250 ms) and the hitch frames since the last clear; `tools/harness/scripts/hitch_ladder.satcmd`
steps through every time rate (1x .. 1 yr/s). Work that used to stall a frame now runs elsewhere or within a budget:
rocket plume parcels are built on worker threads (from 2 min before lift-off); new launch batches are built in
parallel, at most ~8 ms of building a frame, with pad deconfliction read from a per-site, per-day index; the Kessler
cascade's output is mirrored within 3 ms a frame (sim time holds at the mirrored frontier, with the banner, instead of
a frame freezing); the debris pool's CPU arrays are reserved at load; the ambience table and bookmarks load during
the loading screen.

## History
Measured 2026-10-20 (RTX 3070 Ti, 1600x900, default settings, default start): right after load the first frames took
6 x 100-140 ms (launch batch builds; first frame 390-570 ms), 1 min/s - 1 h/s had 80-220 ms frames whenever a launch
first mattered (plume parcels integrated on the main thread), 1 mo/s and 1 yr/s 80-300 ms frames (batch builds with
a 72-h ship-landing scan per Starship, pad deconfliction enumerating ~2000 planes per build, the Kessler sync of
hundreds of thousands of pool changes, the pool arrays reallocated a slot at a time). After: the first frame
~270-390 ms (one-off), every other rate's max under ~60-90 ms, mostly GPU-bound frames; see the final tables in the
2026-10-20 hitch report.
