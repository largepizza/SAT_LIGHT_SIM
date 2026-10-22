---
pages: [rendering/clouds/march.md, rendering/clouds/field.md, rendering/clouds/weather.md, development/profiling.md, development/harness.md, development/tools.md]
code: [shaders/include/cv2_cdc.glsl, shaders/cloud_v2_cdc.comp, shaders/cloud_v2_froxel.comp, shaders/cloud_v2_march.comp, shaders/sat_sky.frag, shaders/cloud_v2_noise.comp, tools/make_mie_lut.py, src/simulations/SatelliteSimCloudsV2.cpp]
---
## Now
The clouds' flash buffer (the Sun shadow lattices: fine grid, cascade, shaft rings, ~5.5 MB) lives in device-local,
host-visible memory when the device offers it (system RAM otherwise; `SATLIGHTSIM_FLASH_HOSTMEM=1` forces it): every lit
cloud sample and the sky pass's god rays read it, and in system RAM those reads crossed PCIe (sky pass -0.9..-2.3 ms).

The Weather tab's "Rebuild experiments" section holds six experiments, all off by default (settings.json `clouds_v2`):
- "Occupancy clipmap" (`cdc_skip` 0 / 2): an eye-centred clipmap (8 levels of 128 x 128 x 64 cells, 100 m doubling,
  320 m tall, on the layer shadow map's anchored plane) holding the max coarse cloud extinction over 36 points of a
  2-cell box; the view march's coarse steps skip the field in known-empty cells. -4..-9 ms of cloud march below orbit at
  full rate, ~0 from orbit; images at the noise floor; off above 4x time. 96 MB, allocated on first use.
- "Light march from clipmap" (`cdc_light`, `cdc_light_levels` 3): light steps 3+ use the cell's mean and variance of the
  light extinction through a gamma model E[exp(-L e)]. Modest (up to -1.4 ms).
- "God-ray froxels" (`godray_froxels`): the clouds' Sun shadow along the view on a 160 x 90 x 64 screen-tile x log-depth
  grid (50 m - 400 km), filled once a frame; the sky pass reads it instead of 3 cascade + 24 shaft lookups a step. Sky
  pass -0.2..-2.9 ms, images at the floor.
- "Mie phase" (`mie_phase`), "Mie optics" (`mie_optics`), "Droplet radius (um)" (`droplet_reff_um` 10): a Lorenz-Mie
  phase table for water droplets (`tools/make_mie_lut.py` -> `assets/textures/cloud_mie.rgba16f`: 2048 angles x 12
  effective radii 4-30 um, gamma size distribution, 400-700 nm against sunlight into linear sRGB; g 0.84-0.88, cloudbow
  37-40 deg from the antisolar point, glory). "Mie phase" replaces the single-scattering phase (clouds read flatter:
  ~20x less side scattering than the tuned pair); "Mie optics" multiplies the tuned phase by the table's structure
  (corona, cloudbow, glory) and keeps its energy.
- "Cap lobes" (`shape_caps`): the shape volume's lobe channels as rounded caps x / sqrt(x + 0.04), x = 1 - (F1/0.7)^2,
  remapped to the cones' mean and spread: rounder cauliflower lobes, clouds in the same places.
- "Full-resolution clouds (probe)" (`full_res`): the whole half-res cloud chain at full resolution; with a sparse march
  the march costs what half-res full rate does, the composite is the extra cost.

Harness: `state` -> `clouds_v2.pass_ms` (per-pass GPU time in the cloud bucket) and `clouds_v2.eval_count` (field
evaluations per caller, from shader variants compiled with `-DCV2_EVAL_COUNT`).

## History
Audit and slice plan: `.plans/CLOUD_REBUILD_PLAN.md`, measurements in `harness_runs/s0/summary.txt`. Coarse view steps
are 70-94% of field evaluations but cheap in empty air: from orbit skipping 20 M of them saved 0.2 ms. Tried and dropped:
bakes at a coarse footprint (more misses), jumping to cell exits (no gain), skipping fine steps / light samples / the
ground shadow in empty cells (no gain), a light march from the cell MEAN alone (up to -18 levels darker: Jensen).

## Now (update, 2026-10-22 later): "Full-resolution clouds" is a real mode
`clouds_v2.full_res` (Weather tab "Full-resolution clouds", off by default) no longer runs the whole half-res chain at
full res. The v2 march and resolve run on the full render grid (always sparse: 1 pixel in 4 a frame, the ray count of the
half-res full rate); `cloud_march.comp` still composites aurora, beams, plumes, dust and the ground shadow at half res and
writes how its output depends on the cloud; a new pass, `cloud_v2_combine.comp`, writes the cloud targets at full res by
putting the full-res cloud back in to first order (exact for the cloud alone). Measured at 1920x1009 against half-res full
rate: +1.1..+4.2 ms of cloud time, visibly sharper; motion equal except fast flight inside cloud (better than half-res
sparse there).

## Now (update 2): defaults
Full-resolution clouds are ON by default on High and Ultra (off on Low, Medium, Planetarium, Potato). The cloud noise is
read at the half-res pixel's level of detail in this mode (the full-res pixel flickered from orbit). The temporal
resolve smooths the sparse grid's samples with a [1 2 1] tent wherever it has no usable history, at any resolution
(fast flight inside cloud: 7.9 -> 4.9 on the motion benchmark at half res).

## Now (update 3): clipmap and froxels on; anvil puffiness
The occupancy clipmap and the god-ray froxels are on by default. "Anvil puffiness" (Clouds tab, storms, `anvil_puffiness` 1;
"Anvil top lift (m)" `anvil_lift_m` 800): an anvil (a tower's flared head) keeps its lobes and has its top raised in billows
by them, with an edge that thins over up to 800 m; 0 is the old smooth head. Its erosion stays faded on the top.

## Now (update 4): one Sun shadow (the Sun transmittance volume)
"Sun shadow volume" (`sun_shadow_volume` 1, on): the clouds' shadow on the clouds, the air (god rays), the fog and dust and
the ground comes from one structure: 4 nested levels of 128 x 128 rays toward the Sun (250 m to 16 km apart, reaching ~14
to ~900 km), each holding the Sun's transmittance at 48 heights on its sunward side. It replaces the fine shadow grid, the
two shaft rings, the cascade and the layer shadow map. Against a brute-force reference it is closer in every view tested
(a cumulus field under a storm: 2.4 levels mean error against 32), costs about the same, and moves better (motion benchmark
2.95 -> 2.21).
