---
pages: [rendering/clouds/march.md]
code: [tools/make_ice_optics.py, shaders/include/cv2_optics.glsl, assets/textures/ice_optics.rgba8]
---
## Now
The traced ice-halo table `assets/textures/ice_optics.rgba8` is 244 x 277 x 43 RGBA8 (11.6 MB): the light's
elevation over the crystals' horizontal runs 0..20 deg at 1 deg, 22.5..60 at 2.5, 65..90 at 5. Seen from above a
cloud deck with plate crystals, the subsun (2e below the Sun) and the two subparhelia (under the sundogs) are
vertical streaks: they are reflections off the plates' basal faces, so the plates' 1-deg wobble moves them 2 deg in
elevation per degree of tilt and hardly at all in azimuth at a low Sun. Their bright core spans ~9-10 deg of
elevation at every Sun elevation.

## History
2026-11-11, issue_20261111_164656 "Weird optic" (three vertical lines under the Sun from 11.9 km): the features are
physical. The table had 2.5-deg layers everywhere; the subsun moves 2 deg per degree of Sun elevation, so between two
layers the trilinear blend drew two lobes 5 deg apart, a streak up to ~40% longer that pulsed as the Sun rose. The
1-deg layers below 20 deg removed that (re-traced, 17 min on the CPU; shared layers differ by Monte-Carlo noise only).
