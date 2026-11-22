---
pages: [rendering/hardware-tiers.md, rendering/atmosphere-and-sky.md, rendering/launch-visuals.md, using/graphics-settings.md, development/harness.md]
code: [shaders/sat_sky.frag, shaders/include/sky_atmos.glsl, src/simulations/SatelliteSim.h, src/simulations/SatelliteSimLaunches.cpp, src/simulations/SatelliteSimNLC.cpp, src/simulations/SatelliteSimUI.cpp, src/simulations/SatelliteSimHarness.cpp]
---
## Now
Planetarium draws no rocket plumes: a launch there is the rocket's point, its 3D model, its engine flame (four
Gaussians) and its engine light. No exhaust parcels are built or walked, no plume bricks or launch-made noctilucent
clouds are made (`SatelliteSim::launchPlumesDrawn()`, false on Planetarium and Potato); Settings > Launches carries a
note there. The Sun's Fury's dust clouds are still drawn on Planetarium.

Planetarium's sky shader (SKY_LITE) compiles no flat cloud layer and no night city detail texture: no cloud setting
draws anything on Planetarium (Settings > Clouds says so; its tab label is dimmed), and its night cities are the
night-lights map under the city light sprites; the tiled day detail texture stays. It integrates the Sun's
transmittance over each atmosphere step like the full sky (`atmStepAttn`, whole steps) and marches 5-10 view samples
(`clouds.view_samples_min` 5 from the preset). SKY_LITE is 217,088 B of its 230,400 B SPIR-V budget (13,312 B free).

Harness: `shaders reload sky_lite=<spv>` rebuilds Planetarium's pipeline from another SPIR-V file (its driver
statistics follow `lite:` in `sky_stats`), so SKY_LITE variants are timed against each other in one app.

## History
2026-11-22 (issue_20261121_160405, the user's decisions). Near a twilight launch the plume pass was 9.8 ms of
Planetarium's 13.3 ms GPU frame; it is 0.4 ms (the frame 4.4 ms). The flat cloud layer was 21.7 KB of SKY_LITE and
drew nothing at the preset's coverage 0; cutting it paid for the step integral (+10.0 KB). Twilight at 10 km, Sun
-3.5 degrees, against Medium with no clouds, aurora or airglow: 30 degrees up 23.8% -> 1.3% off (the red), 10
degrees up 7.8% -> 2.5%, the horizon band 3.5% -> 4.5%. The rings round the zenith (one per change of the whole-number
step count): the largest brightness step 3.26 -> 1.25 levels from 10 km, 4.18 -> 1.04 from the ground. Sky pass cost:
+0.3-0.5 ms at twilight, under 0.15 ms elsewhere. The 2026-11-21 figure for the old shader (48% too red) was measured
against a Medium frame whose knocked-out clouds left a stale Sun shadow volume in the sky; Medium is only a valid
reference there with `clouds_v2.sun_volume 0`. A dense city from 2 km at night is now a smooth bright sheet of
night-map light with the sprites on it (the detail texture drew street blocks with dark gaps): the lit ground near a
populated coast is about twice as bright as before.
