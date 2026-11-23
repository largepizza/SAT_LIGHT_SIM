---
pages: [rendering/terrain.md, rendering/atmosphere-and-sky.md]
code: [shaders/include/terrain_detail.glsl, shaders/sat_sky.frag, shaders/sky_taa.comp]
---
## Now
Terrain silhouettes carry analytic coverage when the sky TAA is on. `sat_sky.frag -DSKY_TAA` measures, for a ray
that missed (or hit something 15% farther than the nearest surface round its pixel, the half-res seed), how closely
it passed over that surface: `tdEdgeCoverage` scans the TRUE height along the ray past the seed distance (32 points
two footprints apart, then a golden section round the best) and takes the smallest gap in pixel footprints. The march
counts a pass within one footprint as a hit, so its silhouette is the e = 1 line: the pixel stores e - 1 (in this
render's pixels when upscaling) in the colour alpha (0.98-0.995) where it is under 1. `sky_taa.comp` blends such a
sample toward its world-down neighbour (the ENU up projected at the pixel) by 1 - e, before its neighbourhood
statistics, at 100% and when upscaling. An edge is thus a 1-px ramp on every frame, including frames without usable
history (motion, flashes). The Moon's alpha marker range is now 0.96-0.978. Cost ~+0.2 ms (Chamonix, 1920 x 1009); the
TAA-on sky fragment shader compiles to 168 registers instead of 255. Not covered: TAA off (no pass reads a neighbour)
and Planetarium (SKY_LITE has no terrain march).

## History
2026-11-23 (wave c26, .plans/TERRAIN_EDGES_PLAN.md phase 1.1): the user: "The terrain edges have always been poor,
even in sunlight." Phase 0 (c24) had shown daylight edges are the march's 1-px aliasing, softened only by the TAA's
history. Tried and dropped: closest-pass candidates from the march's own samples (lower bounds, a different bump),
end-bracket searches (the gap is bumpy at the footprint scale), a 20-footprint window, refinement inlined at the
march's step sites (+23% code, +10 ms), counting every grazing bump on a slope (blurred whole slopes).
