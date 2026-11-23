---
pages: [rendering/clouds/temporal.md, rendering/hardware-tiers.md, development/harness.md]
code: [src/simulations/SatelliteSimCloudsV2.cpp, src/simulations/SatelliteSim.h, src/simulations/SatelliteSimUI.cpp, src/simulations/SatelliteSimHarness.cpp]
---
## Now
From orbit (above `full_rate_above_km`, 30 km) the clouds are marched one ray per 3x3 block a frame ("the orbit
block", `clouds_v2.sparse_block_orbit` 3). Its gate sorts motion by what the history loses to it
(`clouds_v2.orbit_gate` 1; no sliders, the keys are in settings.json):

- A **turn** reprojects exactly, so the block rides it: up to `orbit_turn_px_s` 480 cloud-grid pixels a second and
  `orbit_turn_px_frame` 16 a frame (at a 46.5 deg field on a 1009-px grid a pixel is 0.049 deg: 23 deg/s at 60 fps,
  16 deg/s at 20 fps). Pixels turned above the limit are a debt that drains at the limit; past `orbit_burst_px` 64
  the block is off (the 2x2 grid's held pixel) until the view has not turned for `orbit_wait_frames` 16 frames and
  `orbit_wait_s` 0.8 s. A mouse nudge or a small flick never leaves the block.
- While the block turns, the resolve's accumulation cap (`sampler_accumulation`, 32) is `orbit_turn_accum` 8 on the
  turning frames, ramped in over the first few (a single nudge leaves it at 32). A pixel's own ray comes every ninth
  frame; at the still cap it weighs 3% and the eight Catmull-Rom resamples between rays compound for ~300 frames.
- A **jump** (history lost), an eye step or a time-warp slide past 0.1 pixel a frame: `orbit_jump_frames` 64 frames.
- A **zoom**: `orbit_zoom_frames` 32 frames and `orbit_wait_s`.

`orbit_gate` 0 is the gate of 2026-11-09: 64 quiet frames, any turn past 0.3 pixel a frame or any zoom restarts the
count. The harness `state` lists `clouds_v2.orbit_gate` (`turn_px`, `debt_px`, `over`, `still_frames`, `still_s`,
`since_jump_frames`, `since_zoom_frames`, `turn_load`, `accum_now`) beside `orbit_block` and `sparse_block_now`.

Measured (RTX 3070 Ti, 1920x1009, issue_20261122_114811's view from 1037 km, High, time at 1x, GPU frame): settled
48.3 ms; a 1-pixel mouse move every 30 frames 62.0 on the old gate, 48.0 on the new; Medium 35.2 -> 27.5 (settled
27.0-27.7), a 5 deg/s pan on Medium 34.1 -> 26.3. After a 1-s 90 deg/s pan the block is back 16 frames after the stop
(0.96 s at 16.6 fps) instead of 64 (4.1 s). Moving frame against the settled view, last frame of a 2-s pan, mean
|diff| in levels, old gate (2x2) / new gate (3x3): at 20 fps 0.5 deg/s 0.86 / 0.89, 2 deg/s 0.91 / 1.00, 5 deg/s
0.89 / 0.97, 10 deg/s 0.88 / 0.99; at 60 fps 2 deg/s 1.08 / 1.12, 5 deg/s 1.08 / 1.21, 20 deg/s 1.04 / 1.16; a
90 deg/s pan is on the 2x2 grid in both.

## History
2026-11-22, issue_20261122_114811 "Low FPS in LEO regression" (17 fps on High at 1037 km). No build had made the view
slower: the shot was taken 29 frames after the camera stopped, on the 2x2 grid. Since the first-person camera
(2026-11-17) every mouse motion turns the view, and one mouse pixel (1.9 cloud-grid pixels) was six times the old
gate's turn threshold.
Keeping the block through a turn with the still accumulation cap was tried first and is too soft: after 2 s of a
2 deg/s pan at 60 fps the frame is 1.65 levels from settled against the 2x2 grid's 1.00 (0.7% of pixels past 16 levels
against 0.1%), and it grows with the frames turned, not with the rate (40 frames: +0.2, 120 frames: +0.6). With the
cap at 8 on turning frames it is 1.04 (cap 16: 1.35, cap 4: 1.08).
The wait after a fast pan was scanned at 9 / 16 / 24 / 32 / 48 / 64 frames: 16 and longer are within 0.05 levels of
each other at every frame after the stop, 9 is 0.1 worse. After a jump the frames are 1.4 to 3.2 levels from settled
60 frames on, in two modes over identical trials, and waits of 20 and 32 frames were in the worse mode more often
than 64: the jump's wait was left at 64 frames.
