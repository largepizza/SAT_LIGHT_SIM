---
pages: [rendering/clouds/march.md, rendering/clouds/temporal.md, history/known-issues.md]
code: [src/simulations/SatelliteSimCloudsV2.cpp, src/simulations/SatelliteSim.h, shaders/include/cloud_lightning.glsl]
---
## Now
The Sun transmittance volume (the clouds' one Sun shadow: the key light beyond the light march, the god rays, the
fog and the ground's cloud shadow) is baked at every time rate. Time running never switches it off, on any graphics
tier. Only a jump of the clock restarts it: the part of a frame's sim step that the time scale did not run (a time
set, a scrub, a bookmark's or an issue report's time) when the screen history drops for it too
(`clouds_v2.history_time_jump_s`, 30 s), it is longer than `clouds_v2.sun_volume_max_step_s` (1 s; Ultra 1e6, no
UI) and longer than 4 times the frame before's. A camera path's or a cinematic's sim channel steps the clock every
frame without the time scale; that counts as time running (no restart up to 30 s a frame, one where it starts
above that).
Past 600 s of sim a frame (1 day/s and up) every cloud history restarts each frame and the volume is off. Below
that the volume trails the clouds and the Sun by its rounds (32 to 64 frames, about 2 s of real time at any rate;
at 1 h/s that is up to two hours of sim, so shadows there are present but late). `clouds_v2.sun_volume_timelapse`
(1; no UI) set to 0 brings back the old cut-off, for an A/B.

Known limit (open): with the Sun within about 2 degrees of the horizon the volume lets light through a storm tower
seen against the Sun (its knots are by height, so along a grazing ray one knot spans 340 m / sin(elevation), about
19 km at 1 degree): an orange glow at the tower's base and sunlit deck tops in its shadow, with time held.

## History
From 2026-11-08 to 2026-11-21 the volume was switched off while any frame advanced sim time by more than
`sun_volume_max_step_s`, i.e. at 1 min/s from 30 fps down, on every tier but Ultra. Under a storm at sunset the
ground and the cloud undersides lit up orange and the Sun's glow stood on the horizon, grey again the moment time
was paused (issue_20261121_162947 "Storm lighting": ground box 27.9 levels at 1x, 37.6-39.7 at 1 min/s, 28-33
after). The cut-off had been added for storm shadows trailing growing towers at 5 min/s, measured with
`tools/harness/artifacts.py`, whose still scenarios jump the clock instead of running it (the path's sim channel
is only on the second key). With time really running, on the ITCZ report's view, keeping the volume is better at
every rate: tiles off the settled image by more than 10 levels 29 -> 25% at 1 min/s, 27 -> 18% at 5 min/s, 5.5 ->
2.4% at 1 h/s, and the worst tile 127 -> 60, 134 -> 27 and 15 -> 13 levels. It is also cheaper under the storm at
1 min/s: GPU 37.7 -> 32.0 ms (the light march no longer lights every sample itself). Keeping only the coarse
levels in time-lapse was tried and dropped (more tiles off than either: 45% at 5 min/s).
