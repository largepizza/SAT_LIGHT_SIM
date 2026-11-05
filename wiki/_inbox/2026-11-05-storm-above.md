---
pages: [rendering/clouds/storms.md]
code: [shaders/include/clouds_v2.glsl, shaders/include/cv2_storms.glsl, shaders/cloud_v2_march.comp, src/simulations/SatelliteSimCloudsV2.cpp, src/simulations/SatelliteSimWeather.cpp]
---
## Now
The anvil's thin-ice veil (beyond the plate) is lit as a deck: its Sun self-shadow is the slant path to its top,
sigma x (top - h) / mu0 x 0.5, not a fixed 400 m, so under a grazing Sun it is no brighter than the plate beside
it. It fades out over the plate's presence 0.46-0.66 (the plate reaches its density by ~0.6), so no gap opens
between them. A storm's shield (not a cyclone's) has a sharp upwind edge (the cap behind its core squashed x1.8
along the spine) and a wandering outline (u x 1..1.45 from three sines, wavelengths 0.7-2 radii, in the shield's frame, seeded per storm via
the fraction of b.w). A hurricane's CDO plate takes "Hurricane top density" (`clouds_v2.storm_cyc_top_density`)
12 x the anvil ice (range 1-20). The march's shell reaches "Cb overshoot" + 800 m over the tropics' anvil lid, so
overshooting domes are not cut flat.

## History
2026-11-05 (storm-above): from orbit at sunset every anvil was a "crater" (bright veil halo, dark gap ring, grazing-lit
plate). The hurricane's "whiter plateau" by the eye was the low CDO mass seen through a translucent plate (top
density 3, and the setting was clamped at 6), not the eyewall heads. Black dome blots over the tropics were the
shell's top cutting the domes (hi = Cb top x 1.35 only).
