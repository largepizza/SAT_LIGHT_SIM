---
pages: [rendering/hardware-tiers.md, rendering/clouds/march.md, using/settings.md, rendering/cities.md, using/graphics-settings.md, using/getting-started.md]
code: [src/simulations/SatelliteSim.cpp, src/simulations/SatelliteSimCloudsV2.cpp, shaders/cloud_v2_march.comp, shaders/sat_sky.frag, shaders/sky_taa.comp, src/simulations/SatelliteSimTutorial.cpp]
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

## History (city light flicker, same day)
With temporal anti-aliasing on, distant city lights blinked from frame to frame (the user's Dallas and Arabia
snapshots). The light sprites were depth-tested against the jittered ground depth and z-fought it; they now test at
0.95 of their range. The ground's point lights were hit or missed by the sub-pixel jitter and the resolve clipped
their history away; they are now drawn at the unjittered pixel. Flickering pixels at Dallas from 3.5 km 1.2% -> 0.67%.

## Now (presets, same day)
Medium renders at 67% and Low at 50%, both with every effect on (Low used to switch off the aurora, Reflect beams,
fog/dust and terrain detail); Planetarium is the tier with everything off. GPU frame on an RTX 3070 Ti at 1600x900 in
the user's storm snapshots: Low 10-14 ms, Medium 14-18 ms.

## History (city lights, evening)
The user's snapshots over SoCal from 1805 km (field of view 36 degrees, flickering; 2.5 degrees, steady): pixels
flickering by more than 8 levels 0.87% -> 0.02% at 67% render scale, 0.26% -> 0.02% at 100%; Dallas from 3.5 km
0.17% -> 0.07%. The earlier fix had shifted only the glitter points back to the unjittered pixel; the lights were
being switched by the night map at a city's faint fringe (its value minus the map's blue base crosses zero, and the
sparse fringe's light is concentrated into a few very bright points) and by the terrain contour (a 120-m cut, with the
jitter moving the height sample 100-300 m on a slope). Integrated GPUs had started on Low, which since the same day
renders every effect at 50%.

## Now (final, late evening)
Distant city lights under temporal AA: when nothing moved in a frame (the camera stood still and sim time runs at no
more than ~1.5x), the temporal AA keeps its full history instead of clipping it to the current frame's neighbourhood,
and its "flash" rule ignores a single bright sample; a light smaller than a pixel is then averaged over all the
sub-pixel positions instead of blinking, and lone glints on the sea no longer flash. In motion the temporal AA is as
before. The city lights read the night map, the terrain contour and their own pattern at the pixel's unjittered
position where the view meets the ground at more than ~12 degrees; first runs on integrated GPUs start on Planetarium,
with a tutorial card that says there are no clouds and points to Low.

## History (late evening)
Pixels flickering in four still views (Irvine horizon from 330 m, SF Bay from 1.2 km, SoCal from 1805 km, Dallas from
3.5 km): before the day's work 2.31 / 0.58 / 1.07 / 0.94%; the evening build 4.63 / 0.32 / 0.02 / 0.07%; final 0.57 /
0.04 / 0.13 / 0.04%. Several intermediate versions decided "still" per pixel (from how far it moved on screen) and
relaxed the history rules there; climbing over clouds the distant clouds counted as still and left trails (climb
benchmark 1.60 -> 3.87), so all of that was reverted; the final version only relaxes them when the whole camera is
stationary. The motion benchmark now runs over clouds (the earlier one was a clear night at a Reflect site).
