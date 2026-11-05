---
pages: [rendering/clouds/march.md, history/design-notes.md]
code: [shaders/cloud_v2_march.comp, shaders/include/cloud_lightning.glsl, shaders/cloud_v2_lightning.comp]
---
## Now
The cloud march's light march is unchanged: 6 equal steps over "Light march length" (steps 0-1 on the field with
detail, 2-5 from the clipmap's S2 means where valid), a far sample at 1.2-2.0 x the length, then the Sun
transmittance volume (S3) for everything beyond 2 x the length. The S3 volume is a far-shadow structure: its 48
knots per ray are by HEIGHT (~340 m apart), which along a ray under a low Sun is ~340 m / sin(elevation), about
10 km at 2 degrees.

## History
2026-11-04: replacing light steps 2-5 and the far sample with the S3 volume's optical depth (one lookup at the end of
step 1) was built and measured, not kept. It saved 1-2 ms of cloud march at three views (cumulus 3 km, storm
20261104_020257 shot 1, sunset anvil 20261104_111947), not the 4-6 ms estimated, because steps 2-5 are mostly S2
clipmap reads already; and it moved the image (+6 levels mean at the anvil, 26% of pixels past 8 levels) because the
height knots smear a tower's optical depth over ~10 km along the ray under a low Sun. A light volume that replaces
light steps needs knots along the ray (by distance). The same day the "dark diagonal bands" seen while walking over
the anvil were traced: not the S3 volume (unchanged with it off) but the light march's grain without history and the
clipmap's motion guard (guard off: 4.30 -> 3.02 levels mean moving-vs-settled).
