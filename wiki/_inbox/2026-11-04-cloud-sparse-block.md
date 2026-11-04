---
pages: [rendering/clouds/temporal.md, using/graphics-settings.md, development/controls-and-settings.md]
code: [shaders/cloud_v2_march.comp, shaders/cloud_v2_resolve.comp, src/simulations/SatelliteSimCloudsV2.cpp]
---
## Now
On the full-res cloud grid the sparse march takes one ray per K x K block a frame: `clouds_v2.sparse_block` 2
(default; one pixel in 4), 3 (1 in 9, a Latin-square order of the nine offsets) or 4 (1 in 16, Bayer order). No
slider; the half-res grid and the full / half / adaptive rates stay 2 x 2, and smooth tiles run only at 2. The
resolve keeps a moving pixel's history when the history's resolved distance (carried in the age image's y channel,
RG32F) matches the reprojected distance (`clouds_v2.history_depth_test`, an A/B, default 0): past 6-24 px of parallax
such a pixel takes 0.3 of the new frame instead of 0.7, and no widened tent. It helped 4 x 4 (storm walk 1.74 -> ~1.6)
but hurt 2 x 2 (1.36 -> 1.57: lag), so it is off.
Measured (RTX 3070 Ti, render 0.8 of 1920x1009, issue 20261104_111947 anvil at 11 km): cloud march 43 / 29 / 21-24 ms
at K 2 / 3 / 4 still; flying at the player's speed the cloud bucket is 121 / 68 / 47 ms (2.5x the still cost at K 2:
the occupancy clipmap's motion guard switches the skip off). Storm (20261104_020257): 10.7 / 9.8 / 6.5 still, 26 / 16.5 /
13.5 flying. At 10 fps (the anvil reporter's) the 2 x 2 rotation itself shows: marching every pixel halves the
high-pass error (grain, 2-px blocks) on a storm walk. Motion error vs the settled frame (tstab-style): K 3 within ~5-10% of K 2, K 4 +15-25%,
edges in pans +25-50%.

## History
Built to answer "EVE volumetrics marches 1 in 9-16 pixels; are we missing something?": the full-res grid already
marched 1 in 4, and marching every pixel every frame in motion (`full_res_moving_mode` 2) was within ~5% of the 2 x 2
rotation on the same motion test, so the grain and stair-stepped edges seen while moving come from the resolve's
fast-motion path and (at anvils) the Sun shadow volume, not from the amortization. A guided estimate for unmarched
pixels and a 4 / K^2 weight on them were tried and dropped (mixed: storm pans better, anvil pans worse).
