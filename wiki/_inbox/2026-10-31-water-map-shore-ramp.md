# The water map's shore ramp no longer shaves mountainsides (2026-10-31)

`shaders/include/terrain.glsl`, `waterAdjustHeightSurge()` — the one height function every consumer of the
water map goes through (the terrain march, `scene_depth.comp`, `cloud_march.comp`, `sat_sky.frag`,
`terrainHeightAtUV`).

**What it did.** The water map (R8G8, `tools/make_water_map.py`) stores a signed shore distance whose texels
are 8192x4096 — ~4.9 km at 45 degrees — even though the shoreline inside it comes from Natural Earth 10 m
rasterised at 16K. Two things were driven by that distance, `d` (m, > 0 in water):

- `d > 0` = water, height = the nearest body's level (`wm.g`). Unchanged.
- `d < 0` = land, and the land was capped: `min(land, level - kShoreSlope * d + 1e4 * smoothstep(kShoreRampM,
  2 kShoreRampM, -d))` — a ceiling of 0.25 m per metre from the *mapped* shore for the first 1.5 km inland,
  released between 1.5 and 3 km. The intent was to smooth the DEM's 35 m land baseline step at a coast.

Where the mapped shore is a kilometre or two from the real one (a fjord coast, an archipelago, a lake finer
than a texel) the cap is not smoothing a step, it is flattening the mountainside: land 1.1 km "inland" of the
mapped shore was forced to `0.25 x 1100 = 275 m` whatever the DEM said. The result was a flat, hard-edged,
teal plane lying on a slope thousands of metres high — the southern Andes report (issue_20261030_204632), and
it would show at any ~1 km/m coast finer than a mask texel.

**What it does now.** `if (hDem >= level + kShoreCapMaxM) return hDem;` with `kShoreCapMaxM = kShoreSlope *
kShoreRampM` (375 m): above the ramp's own reach, that curve is not this pixel's shore, and the ground keeps
its own height (no bank, no cap). The cap keeps working where the DEM really is a coastal step.

**Measured** (report view, `report load ... shot=1`, 1920x1009, render scale 0.80):
the slab pixels sat at 299.1 m where the DEM is 835-849 m (0.25 x 1111 m of mapped shore distance + 11 m of
detail = 299 m); they now stand at the DEM. Slab area over lat 44.55-45.15 S / lon 73.35-72.55 W, measured from
the water map + DEM through the same height function, 68.8 -> 20.9 km2 (-70%), deepest cut 889 -> 369 m. Water
in the same region unchanged (178.4 km2); water pixels in the frame 21725 -> 21727. A daylit control coast
(Namib, `time sun 35 setting`) differs by mean 0.234 levels. 87-90% of the ramp's intended small-cut area
(<= 50 m) still applies at the report region, Big Sur and Lake Geneva.

**Still coarse (not fixed here).** The water map's own resolution: a channel or lake narrower than a texel is
still drawn ~5 km wide with a smooth contour, a flat sea surface and a sand rim (the "pale angular sheets" in
the same report are the Puyuhuapi channel, correctly at sea level, drawn this way). That needs a finer bake of
`earth_water_sdf.r8` / `earth_water_level.r8`, not a shader fix.

Evidence: `qa/waves/20261030b/evidence/c15/` (README with the numbers, before/after crops, slab-mask figure,
control coasts).
