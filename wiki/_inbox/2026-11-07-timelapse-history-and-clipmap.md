# Time-lapse: the cloud history follows in-place weather change; the clipmap stays on with a level floor

Date: 2026-11-07 (branch weather-v2). Pages: clouds (resolve / temporal), performance (occupancy clipmap), harness (artifact gate).

- **What changed (resolve):** with time running faster than ~10x, the cloud resolve treats the weather's own change as change.
  It no longer gives distant clouds the still-view treatment (7x7 clamp box, unclamped history) and its history remembers at
  most ~15 sim seconds. Before, at 1-5 min/s, cumulus that had moved or dissolved stayed as blocky ghosts and a dissolved
  storm tower could stay lit over a sunset (issue_20261107_163510 "clouds leave ghost trails").
- **Measured:** artifacts.py ghosted 32-px tiles 7-12% -> 0 on the cumulus and storm views at 1 and 5 min/s, worst tile 40-109
  -> 4-9 levels; 1x unchanged.
- **What changed (clipmap):** setting `clouds_v2.cdc_timelapse` (default on). Past `cdc_max_rate` the occupancy clipmap stays on
  but reads only levels whose cells live at least 4 frames (1 min/s: level 3; 5 min/s: level 5). Coarse field reads at 5 min/s
  on the tower report: 13.8 M -> 3.1 M a frame; cloud march about -2 ms.
- **Harness:** artifacts.py gained tile-local ghost metrics (ghostT_pct, lagT_max), grain, acc2, the cloud march time per view,
  `--exe` (run another build: perf-frames is the reference for weather v2) and the scenarios cu_trails, cu_horizon, tundra,
  frost.
- **Weather v2 vs perf-frames (same views, the user's settings):** steps 1-3 at 128 march registers, step 4 (the convection
  cube) 237, now 250; forcing 128 spills and is no faster. Time-lapse costs v2 5-15 ms more than perf-frames (cube re-bakes
  ~3 ms a face, the clipmap off).
