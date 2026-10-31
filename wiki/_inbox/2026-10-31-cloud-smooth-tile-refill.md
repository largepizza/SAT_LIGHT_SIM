# 2026-10-31 - clouds v2: the smooth-tile refill waits 16 frames, not 64

**What changed.** `SatelliteSimCloudsV2.cpp`: `kSmoothSettleFrames` (the `cv2EyeStillFrames` gate on
`cv2SmoothNow`, the smooth-tile pass F of `cloud_v2_tiles.comp`) is 16 instead of 64. One constant;
nothing else in the pass, the classifier or the settings moved.

**Why.** The 64 was derived in 2026-10-27 from the tent's residual alone ("at the still history
weight ~0.05, 64 frames leave ~4% of the old history"), to stop `issue_20261027_004019`'s artifact -
after a fast flight the cloud history is the quarter grid's tent, which reads SMOOTH everywhere, so
pass F marches those tiles at a quarter density from a history without their detail: 64-px squares of
the quarter lattice's dots, greyer than their neighbours and missing the cirrus streaks. Measured now
at that report's own view with its own trail captured *while* the flight ends (the eye stops at
playback frame ~198 of 360; the refill is frames 198-240), the blocks appear at a wait of 2 frames and
of 8, and are gone at 16. The classification at 16 frames still reads 53% of a detail's contrast
(0.954^16 = 47% of the tent), over its 30-level threshold.

**Effect (the hurricane view of `issue_20261030_205051`, its own settings).** After a 60 m altitude
step (which resets `eyeStillFrames` like a flight but leaves the history and the occupancy clipmap
alone), the cloud march bucket went from ~20 ms to ~14.2 ms at `eye_still` 16 instead of at 64: the
refill window is 64 -> 16 frames, **3.2 s -> 0.8 s at 20 fps**. Steady state unchanged: image 0.005
levels mean, interleaved pass F / pass A saving 26-27% before and 23.5-26% after (the pairs' spread).

**Rejected alternative (measured).** Classifying pass F from the current sparse grid with a one-frame
lag (the tile test ANDs the history's window with last frame's sparse samples, so a tent history
cannot read smooth) holds the artifact at a wait of 8 frames, but it halves pass F's steady-state
saving at this view (14.7 -> 16.4 ms): the sparse grid's per-sample noise (~10-15 display levels)
trips the 30-level range test in about half the genuinely smooth tiles.

**Not fixed.** A real flight also steps the occupancy clipmap's epoch (`cdc_active` false for ~36
frames, then the cells re-bake), which dominates the same refill at 40-45 ms; that is unchanged. Time
warps >= 30x still never reach pass F (`cv2EyeStillFrames` resets on a > 0.5 s sim step).

Evidence: `qa/waves/20261030b/evidence/c34/numbers.md`.
