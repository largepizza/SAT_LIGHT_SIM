---
pages: [rendering/clouds/temporal.md, rendering/clouds/march.md]
code: [shaders/cloud_v2_march.comp, src/simulations/SatelliteSimCloudsV2.cpp, shaders/cloud_v2_resolve.comp]
---
## Now
With full-resolution clouds the march samples one full-res pixel in each 2x2 block per frame, rotating over the block,
in motion as when still. Each ray is clamped to the scene depth, which is half resolution: on the full-res grid the
depth texel's distance is corrected by the parallax between the depth's own ray (through the half-res texel's centre)
and the march's ray, taken on the sphere through the depth's hit (exact for flat ground at any height, bounded to
+-25% of the depth so a ridge silhouette bleeds at most that much on the half pixel it straddles). The adaptive rate and
the half-rate checkerboard never run on the full-res grid. Setting `clouds_v2.full_res_moving_mode` (no UI row, an A/B):
0 the sparse rotation (default), 1 the sample held at one pixel of each block, 2 every pixel marched while the eye
moves from above `full_rate_above_km` (+1-6 ms GPU there, no visible gain).

## History
Issue 20261022_210358 ("Tremendous flicker when moving above clouds", 1278 km at ~700 km/s, Medium preset) and
20261022_220214 ("Continued flicker from orbit"). In motion, sparse full-res clouds flickered (3.9% of pixels reversing
> 12 levels a frame) and whole distant cloud systems went missing (far band -18 levels vs a settled frame). The cause:
the half-res depth was traced half a full-res pixel below the even rows' rays, and near the limb from orbit that half
pixel moves the ground by tens of km, so even-row rays stopped short of the cloud. The y offset of the sparse rotation
alternates each frame, so whole systems vanished on every other frame; a still view's sub-pixel jitter hid it. Found by
holding the sample at each offset in turn (an even row never showed the mass, an odd row always did).

Before the cause was found, two workarounds shipped the same day: holding the sample at one pixel (it happened to hold
an even row: no flicker, masses still lost) and marching every pixel while moving (fixed both at ~+4.7 ms, but let the
adaptive tiles onto the full-res grid: tile-sized squares when slowing down, a misplaced frame of cloud in a fast pan).
After the depth fix the sparse rotation matches full rate (flicker 0.79% vs 0.93% on the 1278-km flight, far band within
~1 level of settled) at 11.7 vs 18.0 ms GPU, so it is the default again.
