---
pages: [rendering/clouds/temporal.md, rendering/atmosphere-and-sky.md, rendering/hardware-tiers.md, using/graphics-settings.md, development/profiling.md, development/harness.md]
code: [shaders/cloud_v2_march.comp, shaders/cloud_v2_far.comp, shaders/sky_ap.comp, shaders/include/sky_ap.glsl, shaders/sat_sky.frag, src/simulations/SatelliteSimCloudsV2.cpp, tools/harness/cloudsky_matrix.py]
---
## Now
Three new `clouds_v2` settings, all ON by default, no sliders: `sparse_block_orbit` 3 (from above `full_rate_above_km`
(30 km), the march runs one ray per 3x3 block while the view is quiet: history valid, no turn or zoom, eye step plus the
volumes' slide under 0.1 cloud-grid pixel a frame, for 64 frames; real orbital speed and 1 min/s qualify, boosted flight, 1 h/s
and pans do not; 2 = off), `far_layer_min_blend` 0.004 (the far cloud layer's pass is skipped while its share is below
this; with the user's 600 .. 8900 km range it starts at ~905 km; 0 = old behaviour) and `sky_ap_limb` 1 (the sky's horizon /
limb band reads a second volume baked by `sky_ap.comp` (320 x 96 x 48 cells, +35 MB, rows uniform in sin(elevation)) instead
of the per-pixel atmosphere loop; 0 = the loop; the loop is still the fallback outside the grid's azimuth range and above
1000 km). The march lights the samples that need a light march first in each batch of 32 (exact, same image), and a faint
fine sample (optical depth under 0.2, no light march) stands for 2 steps (`CV2_FAINT_STRIDE` 2.0, a shader constant; 1.0 =
off; changes the faint veil between puffs' grain: the user picks 2 or 3). A pixel without a ray takes 1 / (K^2 - 1) of its
2x2 weight toward the interpolated estimate in the orbit block (3 / (K^2 - 1) for the still block below 30 km).
Performance (RTX 3070 Ti, 1920x1009, the user's settings, GPU frame): the report issue_20261109_184851's five views (four from orbit, one from 6 km)
paused 39.5-70.5 -> 29.2-38.3 ms; across 57 matrix cells the mean at 850 km 39.5 -> 26.5 ms, at 300 km 30.2 -> 21.7, at 2 m
to 11 km unchanged to -2 ms; still cells over 25 ms 23 -> 10 of 57; cloud bucket 14.8 -> 11.9 ms and sky pass 8.1 -> 6.8 ms
(mean over 54 stills). The limb volume alone: sky pass 10.7 -> 6.8 ms at the worst report shot. Still above 25 ms: dense /
overcast / storm fields from 850 km (27-31 ms), fast flight at 850 km (36-37 ms).
Harness: the `exposure` command (`exposure fast` / `exposure hold`) meters the auto exposure per cell, and
`tools/harness/cloudsky_matrix.py` (gen / score / compare / jumpscore) is the clouds-and-sky matrix (57 cells, ~33 min,
run with `--settings user_data/settings.json`). Timing note for the profiling page: the cloud passes run 20-25% slower in
unexplained phases (after a report load, a shader reload or a cloud setting change, ~200-900 frames), so wait >= 350 frames
and compare best of 3 interleaved reps in one app.

## History
Replaces the 2026-11-08 rule that the settled 3x3 block lost the small puffs from orbit (it was the estimate's share, not the
block). A first cut of the orbit block (8 quiet frames, on through turns) made orbit pans 0.3-0.7 levels worse and was
tightened to "not while turning, 64 frames". Tried and dropped (each measured): a compacted wavefront light pass, shared light
marches from altitude (field reads lighter), a tighter far coarse read, leaving fine mode after 2 empty steps (2x slower),
a 4x4 orbit block (softer). Details: docs/subsystems/clouds.md, render-pipeline.md and perf.md.
