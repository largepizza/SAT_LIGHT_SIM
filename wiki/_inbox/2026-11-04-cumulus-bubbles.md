---
pages: [rendering/clouds/field.md]
code: [shaders/include/clouds_v2.glsl, shaders/cloud_v2_noise.comp, src/simulations/SatelliteSimCloudsV2.cpp, src/simulations/SatelliteSimUI.cpp]
---
## Now
Cumulus bubbles: a cumulus is a cluster of thermal bubbles inside its dome, not one dome per cell. The billow shape
bake (noise mode 4) stores in R the signed distance to a heavy-tailed union of spheres (three octaves: ~200 large,
~1400 medium, ~8200 small per period cubed; at the default shape period and storm scale ~110 m to 1.3 km in radius).
It is read with the turret fetch (no new texture read), so it shares the turrets' upward stretch and lean. A cumulus
point's margin becomes (dome + x bubbles) / (1 + x): the dome still decides where, how big and how high a cloud is,
while the bubbles, turrets and small billow lobes shape its surface at 1 + x times their old weight. The result is
stacked sub-cells of different sizes and heights, heads wider than their stems, hollows and creases between
them, and masses that break into fragments. Settings > Clouds: "Cumulus bubbles" (`cumulus_bubbles`, 3; 0 = the
dome alone), "Bubble relief" (`cumulus_bubble_relief`, 0.2). "Turret stretch" (`cumulus_turret_stretch`) now
defaults to 0.2 (was 0.5). Faded out at a ~1 km footprint, so the far field keeps its dome. Cost: cloud_march
+0.1 to +0.8 ms (1-8%) on four cumulus views (RTX 3070 Ti, 1600x900), still at 128 registers; motion-vs-settled
(tstab) +4-6% on two cumulus views.

## History
Issue 20261104_181014, third round: after the turrets the user said the cauliflower had improved but the clouds still
read as convex 2-D gumdrops. Tried and dropped: reading the meso cells in 3D (they stayed domes, and the march went
to 253 registers); adding the bubbles to the dome without dividing it (smooth "sausage" faces, because a steep margin
hides the lobes); clipping the bubbles to a shell around a shrunk dome (creased facets).
