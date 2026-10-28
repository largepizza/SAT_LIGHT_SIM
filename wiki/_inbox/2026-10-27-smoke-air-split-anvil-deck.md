---
pages: [simulation/wildfires.md, rendering/clouds/march.md, rendering/clouds/storms.md, rendering/launch-visuals.md, development/profiling.md]
code: [shaders/include/cloud_occlusion.glsl, shaders/cloud_march.comp, shaders/cloud_v2_combine.comp, shaders/cloud_v2_march.comp, src/simulations/SatelliteSimCloudsV2.cpp]
---
## Now
Where wildfire smoke lies in front of a cloud, the composite splits the air in front of the cloud in air-brightness space,
and that brightness is the ray's own air: 1 - e^(-0.2 c(t)), c the molecular column from the eye along the pixel's ray
(atmColumn, sea-level vertical columns), inverted by a 14-step bisection between the smoke's and the cloud's distance
(`smokeAirSplitM(eye, dir, tS, tC, kS)`). The full-resolution combine pass takes the camera as push constants
(CloudMarchPC) for it.

A smoke pall (a flat Gaussian, lowered by the ground's drop under its tangent plane at the ray's closest approach so it
follows the Earth's curve) keeps its flat shape when the eye is inside or near it: the drop fades out as the eye comes
within 4 -> 2 vertical sigma of the curved sheet.

Clouds under a higher deck (the ground-overcast model, `clouds_v2.ground_overcast`) take the deck as the map's cloud
above their own top (`f.topH`): the sky light through its diffuse transmission and the sunlight it scatters down.

Storm FPS reference (12.2 km inside a tropical storm complex's cirrus band, the user's High settings, FOV 120,
1920x1009, RTX 3070 Ti): GPU ~33 ms still (cloud bucket 25.5, sky 5.5), ~40 ms moving; removing full-resolution clouds
saves 12 ms, the high layer 10.7, anvil shields 6.8, storm objects 6, Cb columns 3.8; moving costs ~6 ms because the
clipmap's motion guard turns it off.

## History
2026-10-27, issue 20261027_182936 "Orange weirdness from orbit": the split used 1 - e^(-t / 40 km) from the eye; from
orbit both ends saturated and the split clamped at 552 km, so clouds past it seen through any trace of smoke lost the
limb's blue haze: a hard-edged orange-yellow patch the shape of the smoke's reach (it stayed at 0.1x smoke, spectral 0 and
brightness 0). Shot 3 (from 1.1 km inside a pall): the curvature drop at a closest approach ~200 km out put the ray's
smoke behind the horizon clouds, which showed through as a white line along the horizon.
The same day the plumes' sky give-back (`Ls`) was found paired with the wrong `if` (only plumes an engine light reaches
gave back sky light) and restored.
Issue 20261027_183553 "Another region for poor anvil shading": the deck was the map's tau above the sample's height less
the sample's own column estimate; cumulus cores under an anvil lost the deck (blue sky) while their edges took its
diffuse sunlight: dark blue cores with white veins. Shadow mottling (high-pass std) 8.0 -> 4.1 levels, B/R 1.10 -> 1.04,
march -0.8 ms there.
