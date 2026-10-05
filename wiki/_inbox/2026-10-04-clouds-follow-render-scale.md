---
pages: [rendering/hardware-tiers.md, rendering/clouds/march.md, using/settings.md]
code: [src/simulations/SatelliteSim.cpp, src/simulations/SatelliteSimCloudsV2.cpp, shaders/cloud_v2_march.comp]
---
## Now
Render scale (Settings > Display) now also scales the volumetric clouds and the terrain depth pass: they run at half
of the scaled resolution, so at 50% they march a quarter of the pixels. "Clouds follow render scale"
(`display.clouds_follow_render_scale`, default on) turns this off. The clouds' noise detail is filtered at the 100%
pixel size, so their texture is kept, only softer. Satellites, stars, city light sprites, satellite meshes and the UI
stay at full resolution.

## History
Until 2026-10-04 render scale shrank only the sky pass; the clouds and depth pass stayed at half the window, so 50%
bought ~10 FPS. Measured (RTX 3070 Ti, 1600x900, the user's storm snapshots, full-rate march), GPU frame at
100 / 75 / 50%: orbit storm 32.4 / 20.6 / 11.9 ms (50% with fixed-size clouds: 28.0), Dushanbe snow 28.3 / 17.4 / 10.9
(22.6), Arizona anvils 34.0 / 21.8 / 14.0 (28.1). Without keeping the 100% detail footprint the storm cumulus texture
disappeared at 50%; keeping it costs ~0.6 ms there.

## Now (continued, same day)
Below 100% render scale, temporal anti-aliasing becomes a temporal UPSCALER: the background is rendered at the reduced
resolution with a sub-pixel jitter that cycles through 16 positions, and each full-resolution frame is rebuilt from the
current samples and the reprojected history. Detail (terrain textures, patterns) is filtered for the full-resolution
pixel, so 50% keeps most of 100%'s detail where the plain stretch blurred it; it costs ~1 ms more than the stretch.
"Automatic render scale" (`display.dynamic_resolution`, off; "Target frame rate" `dynamic_target_fps` 60; "Lowest render
scale" `dynamic_min_scale` 50%) lowers the scale in 5% steps when the GPU frame runs over 90% of the target's frame time
and raises it when there is room. A scale change keeps the clouds' history (it is resampled to the new size).
