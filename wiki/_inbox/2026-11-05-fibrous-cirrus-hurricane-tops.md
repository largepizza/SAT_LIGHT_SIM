---
pages: [rendering/clouds/field.md, rendering/clouds/storms.md, simulation/tropical-cyclones.md]
code: [shaders/include/clouds_v2.glsl, shaders/include/cv2_storms.glsl, src/simulations/SatelliteSimCloudsV2.cpp]
---
## Now
Cirrus (the high layer, `cv2HighSigma`) is built by `cv2IceFibres`: tufts (a Perlin fBm stretched ~3:1 along the
jet, thresholded, so cirrus comes in patches with clear sky round them) carrying ridged filaments (the same fBm
stretched ~40:1 and ridged where it crosses its median: thin bright lines, dark between), deflected round each tuft's
head so strands bend and fan out like uncinus hooks. Dense heads sit at the top of the layer and their fall streaks
lag downwind more the further they fall (quadratic shear) and drift a little across the wind. Settings > Clouds:
"Cirrus fibres" (`clouds_v2.cirrus_fibres`, 1, 0-2: filament contrast) and "Cirrus hooks" (`clouds_v2.cirrus_hooks`,
1, 0-2: heads and hooked streaks). A hurricane's central dense overcast is a smooth sheet: under it the storm plate's
lobes and erosion are mostly removed, its ice is tripled, and its lid carries a swirl texture (ridged noise in
log-polar coordinates about the eye, ~12 deg inward spiral, turning with the arms) as relief, growing toward the
plate's edge so it frays into streaks. Settings > Weather > Storm objects: "Hurricane top smoothing"
(`clouds_v2.storm_cyc_top_smooth`, 0.85), "Hurricane top streaks (m)" (`storm_cyc_top_streaks_m`, 90), "Hurricane top
density (x)" (`storm_cyc_top_density`, 3).

## History
Before 2026-11-05 cirrus was two octaves of the stretched Perlin thresholded over a soft ramp, combed by a third: smooth
marbled ribbons with no clear gaps ("gooey", the user). The CDO plate used the anvils' billow lobes and erosion: a grey,
dimpled, pitted sheet ("a bumpy mess"). The swirl texture's fetch had to move into the shield walk (cv2ShieldAtR) and
its relief onto the storm margin in cv2Field: in cv2FieldLow either put the cloud march at 248-250 registers.
