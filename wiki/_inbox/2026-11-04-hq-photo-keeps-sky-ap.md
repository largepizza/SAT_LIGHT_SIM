---
pages: [rendering/atmosphere-and-sky.md]
code: [src/simulations/SatelliteSim.cpp, shaders/sat_sky.frag, shaders/sky_ap.comp]
---
## Now
The HQ photo (F8) keeps the aerial-perspective volume (`clouds_v2.sky_ap`) on, like the window: the volume is off only
above 1000 km, on the light tiers (SKY_LITE / Potato), in the environment probes and mirrors. The photo still reads the
Sun shadow volume per sample for the god rays (`godray_froxels` 0 while it settles).

## History
Until 2026-11-04 the photo set `sky_ap` 0 to run "the exact loop in every pixel". At the default 6.5 view samples the
loop is the coarser of the two: at the issue_20261104_020521 sunset storm (822 m) it lit the haze in front of the clouds
~2x as much as a 512-step loop, so the photo's clouds took a light blue-grey veil (cloud mean RGB 128,77,65) that the
window (121,58,41, the volume) and the 512-step reference (120,56,38) do not have.
