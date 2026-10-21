---
pages: [simulation/launches.md, development/profiling.md, using/features.md]
code: [src/simulations/SatelliteSimLaunches.cpp, data/bookmarks/bookmarks.json, tools/harness/scripts/default_bookmarks.satcmd]
---
## Now
When the launch walk finds many batches missing (more than 48 after a time jump: a bookmark Go, `time set`, a Missions
Go to; or any past 1 h/s of warp), it sorts them by urgency (launches nearest now first), builds at most 16 itself
(none at a fast warp) and hands the rest to one background worker thread (below normal priority, all cores but two),
which builds them in chunks; each frame the finished prefix is moved into the batch cache within ~2 ms. The batches
are the same as built on the main thread (`launch list` / `padstats` identical with `SATLIGHTSIM_LAUNCH_BG=0`). A jump
to 2032-08-27 fills all 9706 batches in ~1.1 s (the first ~40 frames, launch CPU ~3.4 ms then 1.9); a bookmark Go fills
in < 40 frames at 1.3-2.3 ms. Plume candidates under 0.25 x last frame's priority cut are not finished (Twilight
Starship bookmark launch CPU 1.77 -> 1.47 ms at 1x). Bookmark 4 "Beam-lit Snow over Antarctica" uses cloud drift phase
1.520: snow at the eye (0.25) lit by the Reflect beams (0.009 Suns).

## History
The hitch pass's ~8-ms-a-frame build budget (2026-10-21) made a jump slow: 2032 had 8.5k of 9.7k batches after 480
frames at ~11 ms CPU, bookmarks took 250-300 frames at 9-12 ms. At 1 yr/s the main thread's builds of cold rival
day buckets reached 62 ms in a frame (hitch_check C 1yr p99 75.2 -> 37.6 ms). Bookmark 4's old phase 1.535 sat at the
shower's edge under the 2026-10-11 cloud defaults (rate at the eye 2e-4); the phase band with both snow and beam
light is ~0.002 rad wide.
