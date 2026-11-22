---
pages: [rendering/atmosphere-and-sky.md, rendering/hardware-tiers.md]
code: [shaders/sat_sky.frag, shaders/include/sky_agx.glsl, shaders/include/sky_atmos.glsl, src/simulations/SatelliteSimCloudsV2.cpp]
---
## Now
Planetarium (the SKY_LITE variant of `sat_sky.frag`) is lit like the full sky: the AgX tonemap, the Sun's exact path
with ozone per atmosphere step, the air's multiple scattering (the Psi table) and the ground's sky light from the Sun
table's sea-level row. It still marches 4-10 view samples with the one-point step form, so its day sky is a little
brighter than Medium's. It reads `cv2.tone` and the two tables through small blocks declared by offset (the full
declarations would cost 10.8 KB of its 225 KB SPIR-V budget), and the Sun table is baked on that tier too. The
multiple scattering is always on there (`clouds_v2.sky_multiple_scattering` 0 has no effect on Planetarium).

## History
Until 2026-11-21 SKY_LITE kept the pre-2026-10-26 atmosphere: `1 - e^-x`, no ozone, single scattering with the
zenith sky standing in for shadowed air. At twilight its sky was grey-green over an orange band where the full sky is
blue-violet (issue_20261121_160405 shot 4): 200% off per channel 30 degrees up, red 59%, green 2%, blue 7% now (airglow off
on both). GPU frame unchanged (4.23 / 3.79 / 4.39 -> 4.35 / 3.54 / 4.42 (run-to-run 0.1-0.4) ms at the report's three views).
