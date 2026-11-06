---
pages: [rendering/atmosphere-and-sky.md, rendering/index.md]
code: [src/simulations/SatelliteSim.cpp, src/simulations/SatelliteSimVideo.cpp, shaders/cloud_v2_march.comp]
---
## Now
Auto exposure ("Auto exposure (day)", `clouds_v2.auto_exposure`) meters the frame WITHOUT the UI: every frame it runs,
the main render pass ends after the scene, the meter's 64x36 blit of the central 60% is taken there
(`recordExposureMeter` from the pre-UI hook), and the UI draws in the load variant of the pass. An open window no
longer changes the exposure (at issue_20261105_181849's view: -1.62 EV with the Settings window open, -1.61 closed; before, opening it lifted the frame +0.52 EV).
The HQ photo, which draws no UI, is metered after the pass as before.

The cloud light march's blue-noise jitter reads its 64x64 tile at a fixed hashed offset per 64x64 block of pixels, so
the tile's own pattern does not repeat across shaded cloud (high-passed autocorrelation at 64 px 0.71 -> 0.01 moving,
0.53 -> 0.01 still). The grain itself is unchanged.

## History
Metered after the UI, the Settings window's dark panel lowered the meter's mean, and the meter (which only darkens a
bright day) darkened 0.5 EV less: clouds ~1.4x brighter while the user tuned them (-1.20 EV open against -1.72 closed),
easing in over ~1.6 s whenever a window opened. The report's "first frames after a reset are dark" is the first
frame's noise: with exposure locked the cloud base's mean at frames 2-300 after a reset is within 1-2%.
