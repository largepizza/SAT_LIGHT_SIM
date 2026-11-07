---
pages: [tools/harness.md, clouds/rendering.md, weather/storms.md]
code: [tools/harness/artifacts.py, src/simulations/Weather.cpp, src/simulations/SatelliteSimCloudsV2.cpp, shaders/include/clouds_v2.glsl]
---
## Now
`tools/harness/artifacts.py` is the artifact gate: report views and the reports' own flights replayed at the user's 30 fps
and time-lapse scales, scored on truly consecutive full-resolution frames for reversal flicker, block structure on the
2/4-px grids (ratio and absolute excess), stair-stepping and drag against the same moment settled. Single-cell storms
ride the wind field (tracks traced when their bin is built), and the storm geometry reads the convection cube back along
the wind by each face's bake lag. While the view moves, the full-res cloud grid's sparse sample is held at one pixel of its
block (full_res_moving_mode 1, any height): the 2x2 rotation no longer crawls over moving clouds. The storm billows' rise
runs on a clock capped at 10x real time.

## History
Until 2026-11-07 every cloud check ran at the harness's 1/60 s step, at 1x or settled, from lossy videos; the user's
1 min/s storms flickered and crawled with blocks that none of them measured. A first gate captured a frame per `capture`
(~5 frames apart) and measured evolution, not flicker.
