---
pages: [rendering/clouds/field.md]
code: [shaders/include/clouds_v2.glsl, shaders/cloud_v2_noise.comp, src/simulations/SatelliteSimCloudsV2.cpp, src/simulations/SatelliteSimUI.cpp]
---
## Now
Bubble hierarchy: the cumulus bubbles come in two sizes that nest. The bubble volume is read a second time 4x finer
(bubbles ~30-330 m at the default shape period and storm scale) and shapes a thin shell around the big bubbles'
surface, so the faces of the big bubbles carry small ones (cauliflower on the cauliflower). The big bubbles' share
follows the cloud's own thickness (full from ~1.8 km, none under ~250 m), so a small cloud is built of the small
bubbles alone instead of being one smooth oval slice of a big one. The bubbles' share fades toward the cloud base
(to 40% at the base, full by 0.4 of the cloud's thickness): one flatter base under separate towers. The bubbles
now have their own gate (any convective cloud from ~60 m thick), so "Cumulus turrets" 0 no longer turns them off.
The bubbles meet in rounded fillets (a smooth union in the bake), not V creases. Settings > Clouds: "Bubble
hierarchy" (`cumulus_bubble_hierarchy`, 1; 0 = one bubble size), "Bubbles follow cloud size" (`cumulus_bubble_size`,
1), "Bubble base flatness" (`cumulus_bubble_base`, 0.6). The values ride in the spare components of the bubbles'
UBO vector (`cu5.zw`). One extra texture read; the march stays at 128 registers; cloud_march +0.0 to +0.6 ms on four
views (RTX 3070 Ti, 1920x1009, contended).

Heavy-tailed cells: the whole-cloud placement bake (meso mode 2, G) has a fourth, rare octave of large masses (8 km
cells at the default 32 km cell period), the blob radii spread down to 0.3 of a cell, and a blob's height follows its
size. Cover is unchanged (numpy check of the thresholded field); cloud sizes form a continuum with a long tail.

## History
2026-11-05, the shape round after "broadly speaking still gumdrops, still very 2D". At the fine bubbles' true
geometric scale the views did not change; their distance is used at 3x. Cloud streets were not built (the meso
volume is read in a drifted ECEF frame, so baked rows would not follow the wind). A `vec4` appended to CloudV2Params
for these values made the app die silently while creating rain_particles.vert's pipeline at boot; packing them into
`cu5.zw` boots.
