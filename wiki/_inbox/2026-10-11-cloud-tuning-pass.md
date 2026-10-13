---
pages: [rendering/clouds.md, rendering/cloud-lighting.md, using/graphics-settings.md, development/harness.md]
code: [shaders/include/clouds_v2.glsl, shaders/cloud_v2_resolve.comp, shaders/cloud_v2_march.comp, shaders/include/cloud_lightning.glsl, shaders/sat_sky.frag, src/simulations/SatelliteSimCloudsV2.cpp, src/simulations/SatelliteSimUI.cpp, src/simulations/SatelliteSim.h]
---
## Now
**Defaults.** The compiled `clouds_v2` defaults are the user's tuning of 2026-10-11 (coverage 0.96 over a
0.0026-1.0 map range, density 1.3, erosion detail 0.77, sun gain 1.9, sky ambient 1.5, light march 8000 m in
7 steps, march budget 430, step growth 0.014, weather evolution window 0.1 h, and ~45 more); the High preset's
march budget / light steps / step growth follow them. Slider ranges widened: light march to 16 km, Cb columns to
4, rain fall speed to 20, evolution window down to 0.1 h, "Map clear below" in 0.002 steps.

**Near-field shafts and haze.** A third Sun-shadow lattice (128 x 128 rays at 250 m, 32 knots along the Sun)
is baked for the god rays and sampled 24 times over the first 14 km of each sky step. "Haze near clouds"
(`clouds_v2.haze_aloft`, default 1.9, 0-20) adds low-level Mie haze that follows the weather cube's cover.
Fog and dust take the cloud shadow from the cascade. "Far cloud shadow range (km)"
(`clouds_v2.sun_cascade_range_km`, 205, 100-800) sets the cascade's half width; the cell and knot sizes scale
with it (410 km = 6.4-km cells, ~20-km knots), and its edge fades over the last fifth of the window.

**Mid layer (altocumulus / altostratus).** "Altostratus density (x)" (`altostratus_density`, 2.5), "Mid layer
thickness (x)" (`mid_thickness`, 1), "Mid layer field size (km)" (`mid_field_km`, 0 = the cluster field's
patches; e.g. 700 gives sheets hundreds of km across), "Mid layer cover bias" (`mid_cover_bias`, sd of its
regime noise) and "Mid layer shadow" (`mid_shadow`, 0.3): the share of the layer's extinction that counts in
the shadow it casts on other cloud (the light march and the Sun shadow bakes).

**Cirrus.** "Cirrus direction warp" (`cirrus_direction_warp`, 0.5, 0-1.2): the streaks' across-wind coordinate is
displaced by five waves of whole counts round the globe, so their heading swings (rms slope ~0.7 x the value)
over 600-5700 km, and the bundles break into patches.

**Shape.** "Erosion size (m)" and "Lobe size (m)" are the detail and shape periods, now in the Shape section.
"Erosion variation" (`erosion_variation`, 0.6) and "Lobe variation" (`lobe_variation`, 0.5) vary erosion
strength, erosion grain and lobe strength by region (one value from the cluster noise at a quarter period).
"Small cumulus" (`small_cumulus`, 0.06, 0-0.3) lifts the low field at the coverage threshold so low puffs
form where the map's coverage is marginal.

**Resolve sampler.** Each half-res pixel carries an age (samples in its history, an R32F image reprojected with
the colour). A young pixel blends at 1 / (age + 1) instead of the fixed history weight and leans on its
neighbourhood's estimate; history removed by the clamp lowers the age. Settings (Clouds > Quality):
"Accumulation (samples, 0 = off)" (`sampler_accumulation`, 32), "New pixel smoothing"
(`sampler_new_smoothing`, 0.8), "Motion clamp (sd)" (`sampler_motion_clamp_sd`, 1.25).
Slider slots 270-282; `kCloudSliderSlots` is 283.

**Light march sampling.** "Light LOD footprint (m)" (`light_lod_footprint_m`) defaults to 0 (off) and is ignored for a light march longer than 3 km.
"Light march footprint (x length)" (`light_sample_footprint`, 0.08, 0-0.2) is the footprint every light sample reads the
cloud field at, as a fraction of the light march's length. Slider slot 283; `kCloudSliderSlots` is 284.

**Multiple scattering and lobe occlusion.** "Multi-scatter octaves" (`ms_octaves`, 2, 1-8): the key light's
multiple-scattering term is the sum over k of strength^k x exp(-optical depth x extinction^k). With 6 octaves at
extinction 0.7 / strength 0.5 it falls off gradually with depth, so a top lit at a grazing Sun is bright and the
lobes keep their relief. "Lobe occlusion (sky light)" (`lobe_occlusion`, 0, 0-1) scales the sky light by the lobe
noise at the sample: darker in the creases between lobes, brighter on their tips, mean about 1. Slots 284-285;
`kCloudSliderSlots` is 286.

## History
- Blue-grey cloud tops under a 10-12 degree Sun (user records 35-36): with "Multi-scatter strength" at 0.026 the
  Sun's light was single scattering only (the top few metres of a flat top) and the sky light dominated. Shadows
  were not involved (cascade, light footprint, mid / high layers, light march length: no change). Top mean
  163/177/206 -> 202/207/223 at 6 octaves, 0.7 / 0.5, relief 2.04 -> 2.08 levels.
- Grain on lit / shadowed cloud (user records 30-33): the light LOD's second step was one point sample for ~7.5 km
  of an 8-km light march, so samples came back sunlit or black. Raw frame-to-frame std 3.69 -> 0.93 levels with
  the LOD off, 0.6 with the 0.04 footprint; settled dark specks 2728 -> 193 at record 32; +2.5-3 ms GPU. The LOD
  was also brighter than the full march (crop mean 173 vs 117), a brightness switch ~5 km from the eye.
- Hard horizontal shadow line across cumulus (user snapshots 23-24): the mid layer's sheet, crossed at a grazing
  Sun over the 8-km light march (optical depth ~38 along the path), shadowed everything at its altitude. Gone
  with the layer off; with "Mid layer shadow" 0.3 the band is lighter but still has an edge.
- Small clouds seen from orbit that vanish on the way down (snapshots 28-29, taken with erosion detail 0):
  climb.py measured cover ~1-2% up to 300 km and 20-30% from 450 km. The sub-pixel cover estimate counts cloud
  well under the threshold while the resolved field does not. With erosion 0.77 the erosion removes those
  marginal puffs at every distance and the climb is consistent (5.5% -> 6.1% across 300 -> 450 km). "Small
  cumulus" is the near-field lever; the far estimate is unchanged.
- Sampler, tstab at snapshot 27 (accumulation off -> 32): pan 2.19 -> 1.90, pitch 1.72 -> 1.36, walk
  3.81 -> 3.54 mean |diff| against the settled image.
- Register cliff: carrying a second regional value down to the erosion block, and a smoothstep form of the
  small-cumulus lift, each put the march at 222 registers; the shipped forms are at 128.
