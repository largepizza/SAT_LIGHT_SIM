---
pages: [rendering/clouds/temporal.md, using/features.md]
code: [src/simulations/SatelliteSimCinematic.cpp, src/simulations/SatelliteSimCloudsV2.cpp, src/simulations/SatelliteSim.cpp, src/simulations/SatelliteSim.h]
---
## Now
An HQ cinematic export settles each frame's pose as a running mean: during the settle frames the clouds' resolve and
the sky TAA take weight 1/n instead of their still weights (`cv2HqHistWeight`, set per settle frame in `cineTick`),
seeded by the previous frame's result as a prior worth one pose of samples (no prior after a cut), and the clouds'
accumulation cap rises to 256 for the export. Each exported frame is the mean of ~2 x "HQ photo settle frames" samples,
half of them shared with its neighbour. Outside an HQ export nothing changes.

## History
At the still weight (0.046) the full-rate march's history remembered ~20 frames, so with 48 settle frames every
exported frame was an independent draw keeping ~15% of the per-sample noise: grain on wispy cloud edges that changed
every frame (worse than the real-time preview; 96 settle frames made it worse still). Real time escapes it because the
sparse grid refreshes a pixel every 4th frame. Measured on broll_earth shot 1 (golden cumulus, 1920x1072, 48 settle
frames), pixels reversing by > 4 levels frame to frame in the cloud region: 0.78% -> 0.25% (preview 0.36%; motion
blur 4 without the fix 0.51%; photo scale 2 with it 0.33% at 3.3x the time). The fade-in after a cut is gone in HQ.
