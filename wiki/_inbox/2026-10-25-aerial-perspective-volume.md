---
pages: [rendering/atmosphere-and-sky.md, development/profiling.md, using/graphics-settings.md]
code: [shaders/sky_ap.comp, shaders/include/sky_ap.glsl, shaders/include/sky_atmos.glsl, shaders/sat_sky.frag, src/simulations/SatelliteSimCloudsV2.cpp]
---
## Now
The sky's single-scattering atmosphere (sunlit and shadowed air, god rays, haze near clouds, city glow, airglow, a solar
eclipse) is computed once per 6-pixel screen tile by a compute pass (sky_ap.comp), not once per pixel. The pass stores
running sums at 48 depths along each tile's ray, per unit of the ray's air column. The sky pass reads them at each
pixel's distance and at the air in front of the clouds. Within a band round the horizon or the Earth's limb, where the
light changes too fast across a tile, the sky pass keeps its own per-pixel loop, and it cross-fades between the two over
the band's outer part. Settings: "Aerial perspective volume (0/1)" (clouds_v2.sky_ap, default 1) and "Aerial perspective
horizon band (deg)" (clouds_v2.sky_ap_band_deg, default 2), in Clouds > Rebuild experiments. The volume is off above
1000 km of eye altitude, on the light tiers and in the HQ photo.

## History
Measured on an RTX 3070 Ti at 1920x1009 (harness_runs/ap/): the sky pass summed over 17 scenes fell from 191 to 146 ms
(-24%). frame_bench wall clock: stills -8.0%, motion paths -8.2% (ground and air views -7 to -14%, the eclipse -20 to
-25%). The pass costs 0.3-0.7 ms. The volume is closer to a fine-step reference than the per-pixel loop at its default
step count (0.01-0.34 vs 0.6-0.8 levels mean). Its first version stored raw sums: from orbit a tile's interpolation read
up to ~9 levels too bright near the limb, which per-unit-of-air storage fixed (0.42 to 0.08 levels mean). A hard band
edge showed a 1-4 level step; the cross-fade replaced it. Four times the loop's steps inside the band matched the volume
but doubled the sky pass from orbit, so the band keeps the default loop.
