---
pages: [rendering/hardware-tiers.md, rendering/clouds/temporal.md, development/controls-and-settings.md]
code: [src/simulations/SatelliteSimCloudsV2.cpp, src/simulations/SatelliteSimUI.cpp]
---
## Now
At the first ascent from orbit (1517 km and 540 km, 1920x1009, RTX 3070 Ti, the user's look on High) a settled frame
costs 48.9 / 44.5 ms GPU and a boosted climb 63 / 61 ms (every climb frame is a history jump, so the clouds run on the
2 x 2 grid). The High preset keeps "Clouds follow render scale" off, so a render scale of 0.8 by itself saves only
3.6 / 2.5 ms; with the clouds following it saves 12.6 / 11.2 ms settled and ~18 ms during the climb. Other measured
options, settled: the 4 x 4 orbit block -9.2 / -7.8, the high layer off -5.8 / -4.6, light steps 4 -4.2 / -2.4 (no
visible change in the crops), coverage 0.42 -6.0 / -4.2. Render scale 0.8 with the clouds following plus the 4 x 4
block reaches 29.6 / 27.9 ms (33-35 fps); dynamic resolution at a 30 fps target sits at render scale 0.55 (24 ms). During
the climb itself only the pixel count lowers the cost.
The harness feeds a fixed 1/60-s frame unless launched with `--fixed-dt 0`: the orbit gate's waits in seconds count
60 frames per second there.

## History
Wave 20261122 c25 (2026-11-23): measured for the user to choose from; no default changed.
