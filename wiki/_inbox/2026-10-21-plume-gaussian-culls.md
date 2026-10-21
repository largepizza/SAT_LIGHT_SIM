---
pages: [development/profiling.md, development/harness.md, "new: rendering/launches.md"]
code: [shaders/cloud_march.comp, src/simulations/SatelliteSimLaunches.cpp, src/simulations/SatelliteSimKessler.cpp]
---
## Now
The dust / plume / flame Gaussian list `cloud_march.comp` walks (`dustCloudsCS`) is culled three ways. A per-tile
cull (`cullDustForTile`): each 16x16-texel workgroup tests every Gaussian's view cone once against the tile's cone into
a list-order bit mask (the image is the full scan's exactly), with a per-Gaussian mask of the engine lights that can
reach it. A per-texel faint skip: a plume whose optical depth at that texel is < 1e-5 and whose light is < 2e-7 of
white at the frame's sky exposure (DustBuf header word 2) is skipped before its lighting (<= 1% dimming and 2e-4 of
white summed over the whole list, worst case). A CPU significance cull (`dustCullInsignificant`) drops the plume
Gaussians whose summed worst-case bounds stay under 2e-4 of white and 1% dimming. Knockouts 16777216 (tile cull +
faint skip off) and 33554432 (CPU cull off) are the A/Bs. `launch state` -> `dust_list` counts the list; `shaders
reload` also reports the composite's register count.

## History
Measured interleaved in one live app (RTX 3070 Ti, 1600x900, cloud_march): droneship 8.1 -> 6.6 ms, 2032 busy era
6.0 -> 5.0, day lift-off 4.3 -> 3.4, night follow at 300 m 3.3 -> 3.0; captures within the noise floor. In the
droneship view 160 of 390 Gaussians (old, wide, Earth-shadowed exhaust) covered every texel while adding ~1e-6 of
white. The launch benchmark's "+22 ms" for those scenes was GPU contention from another app on the same GPU (+3.7 ms
measured alone): time it with no other app running.
