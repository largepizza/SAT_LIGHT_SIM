---
pages: [rendering/clouds/march.md, rendering/clouds/temporal.md]
code: [src/simulations/SatelliteSimCloudsV2.cpp, shaders/cloud_v2_tiles.comp, shaders/cloud_v2_march.comp]
---
## Now
The full-res cloud grid's smooth tiles (32x32 tiles whose last resolved frame varies by less than
`clouds_v2.smooth_tile_threshold`, 30 display levels, in every 4x4 window; marched at a quarter of the sparse grid's
density by march pass F and filled bilinearly) stay on while the eye moves and in fast flight. While moving (until
64 eye-still frames) the threshold is multiplied by `clouds_v2.smooth_moving` (default 0.5, no slider; 0 = smooth
tiles only after 64 eye-still frames and steps <= 150 m, the previous rule). The halved threshold is there because
the classification reads last frame's resolved image at the same pixel, which in motion is partly the quarter
grid's tent and reads smoother than it is.

## History
2026-11-04 (qa motion-rays): a moving view traced ~60% more rays than a still one (anvil of issue 20261104_111947:
360k vs 224k a frame at 1600x900; storm trail 360k vs 158k) because the smooth tiles switched off whenever the eye
moved. With `smooth_moving` 0.5 the moving frame traces 260k / 180k; cloud time while flying -8% (anvil, 60 fps) to
-17% (storm, 10 fps); quality vs settled references at the A-vs-A spread. 1 traces the still frame's rays (about
twice the saving) but drew coarser stair-steps on cloud edges in the 10-fps anvil walk (+6% of pixels past 16
levels). Numbers in docs/subsystems/perf.md "Moving marched every sparse slot".
