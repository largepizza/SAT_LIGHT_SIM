---
pages: [rendering/clouds/temporal.md, development/harness.md]
code: [src/simulations/SatelliteSimCloudsV2.cpp, src/simulations/SatelliteSim.h, src/simulations/SatelliteSimUI.cpp, src/simulations/SatelliteSimHarness.cpp]
---
## Now
After a jump, an eye step or a time-warp slide from orbit, the orbit block (one cloud ray per 3x3 block) comes back
after `orbit_jump_frames` 64 frames, or after `orbit_wait_frames` 16 frames and `orbit_jump_s` 1.0 s if that comes
first (`clouds_v2.orbit_jump_s`, settings.json key, no slider; 0 = frames only). At 60 fps that is still about 64
frames; at 17 fps it is 17 frames (1 s) instead of 64 (3.8 s). Climbing from orbit with Q / E moves the eye 35-190 km
a frame, so every climb frame is a jump; a player who climbs in bursts and stops to look used to spend ~3-4 s after
each stop on the slower 2x2 grid. Harness `state` -> `clouds_v2.orbit_gate.since_jump_s`.

`report load` returns `shot_size`, `window_size` and `window_matches_shot`, and its message warns when the harness
window differs from the shot's frame size. GPU cost goes with the pixel count: the harness default 1600x900 has 74% of
the pixels of a maximized 1080p window (1920x1009).

## History
2026-11-22 (issues 20261122_213304 / _190752, "17 FPS"): the reports' frame times (58 / 66 ms) came from the 2x2 grid
in the first seconds after a climb stopped. The harness numbers that night (34 / 38 ms "settled") were taken at 1600x900;
at the reporter's 1920x1009 the same settled views run 43 / 49 ms. Played back from the reports'
own trails, the first ~3 s after a stop ran at 61-67 ms before; with the seconds-based wait they run at the block's cost.
