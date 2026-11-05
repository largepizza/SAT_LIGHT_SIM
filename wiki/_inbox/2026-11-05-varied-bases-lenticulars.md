---
pages: [rendering/clouds/field.md, rendering/clouds/layers.md]
code: [shaders/include/clouds_v2.glsl, shaders/cloud_v2_march.comp, src/simulations/SatelliteSimCloudsV2.cpp]
---
## Now
Low cloud bases vary. `cv2BaseShift` adds to each low base a regional spread (the cluster Perlin normalised to
+-1.5 x the type's "base variation", 8-64 km) and a lifting-condensation-level proxy: lower over the sea and in rain,
higher over dry land by day (up to ~1.7 km for convective types), and each cumulus cell is offset by its own strength
("Cloud base cell spread", 120 m). The top moves with a lowered base and rises half as far as a raised one. The near
field and the far field use the same function, so bases agree at every distance. Ragged fractus hangs 60-550 m under
raining bases (cloud, not rain, from the rain shaft's own noise texel). The mid layer has a second, low mode: thin
wind-stretched wisps at 1.5-2.5 km over the ground where the low cover is broken. Over mountains, lenticular lenses
form (`cv2OroSigma`): from the elevation map, the relief's excess over its ~25 km mean against the air's dryness sets
a smooth saucer whose height comes from the ~10 km relief dome (the summit pokes into its base), plus one lee lens a
wave length (2 pi U / N) downwind. They are fixed to the ground, not to the drifting map. Settings > Clouds: "Cloud base
condensation level" (`clouds_v2.base_lcl`, 1), "Cloud base cell spread (m)" (`base_cell_m`, 120), "Cloud base
regional spread" (`base_regional`, 1), "Fractus (ragged scud)" (`fractus`, 1), "Mid-level wisps" (`mid_wisps`, 1),
"Orographic clouds" (`oro_amount`, 1), "Ridge wind (m/s, 0 = auto)" (`oro_wind_mps`, 0 = 20 m/s), "Ridge wind
from (deg, -1 = auto)" (`oro_wind_dir_deg`, -1: easterly within 25 deg of the equator, westerly outside), and the
key `oro_moist` (0.6).

## History
Before 2026-11-05 the base-variation term was (cl.a - 0.5) x 2 x alt.w on a Perlin of spread 0.057, so a 300 m
setting moved a base by about +-34 m and every base in a view sat on one line. Inside cv2Field (inlined at every march
call site) the lens code put the cloud march at 251 registers; it is added at the march's one main sample instead.
