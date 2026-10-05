---
pages: [rendering/aurora-airglow.md, development/harness.md, development/controls-and-settings.md]
code: [shaders/include/aurora_oval.glsl, src/simulations/SpaceWeather.cpp, shaders/cloud_march.comp, shaders/sat_sky.frag]
---
## Now
The auroral oval is a ring around the geomagnetic (dipole) pole whose shape is fixed relative to the Sun, in
magnetic local time: widest and furthest equatorward near 23 MLT, thin near noon, dark inside (the polar cap).
The poleward edge is sharp, where the discrete arcs (the sheets) are. The equatorward edge is soft, where the
diffuse aurora is, and there the sheets carry only a quarter of the light. The edges follow an activity index
Kp: at midnight the equatorward edge is at 66.5 - 2.1 Kp degrees of magnetic latitude (Kp 9 puts it near 47)
and the poleward edge at 72.5 - 0.8 Kp; at noon they are 76.5 - Kp and 79 - 0.6 Kp. Brightness is
0.4 + 0.2 Kp (1 at Kp 3).
Kp is a pure function of sim time (`spaceWeatherAt`), built from:
- the 11-year solar cycle (cycle 25 minimum Dec 2019),
- recurrent coronal-hole streams (every 27.27 days),
- CME storms (sudden commencement, a 3-9 h main phase, a recovery of a day or so), which are more likely
  near solar maximum and near the equinoxes (Russell-McPherron),
- substorms (about 2.5 a day, more when active): the oval brightens and bulges poleward around ~23 MLT, then
  the bulge drifts west and fades over ~2 h.

Time reversal, time warp and bookmarks all see the same storm.
Settings (Atmosphere tab, Aurora section):
- "Space weather (0 manual / 1 auto)" `clouds.aurora_activity_auto`, default 1
- "Aurora Kp (manual)" `clouds.aurora_kp_manual`, default 3, range 0-11
- "Storm frequency (x)" `clouds.aurora_storm_rate`, default 1

An "AURORA NOW" readout shows Kp and its G scale, the current driver, any substorm and the oval's latitude.
The harness command `aurora [next <kp>|substorm|kp <v>|auto]` reads or sets the activity, and `state` has an
`aurora` block.

## History
The oval used to be a band at 20 degrees colatitude (+8 x "Storm strength") that faded out over twice its
width. At the default storm level it was lit from ~5 degrees off the pole outward, so from orbit it read as a
cap, not a ring. It was also fixed to the ground rather than to the Sun. "Storm strength" (`clouds.storm_strength`)
is no longer read: the fold chaos and coverage fill are derived as (Kp - 1) / 9, capped at 0.85, because at 1
the coverage gate filled the whole oval.
Calibration: over 2030-2041 the model gives 936 / 501 / 174 / 103 / 9 three-hour intervals per cycle at G1-G5,
against NOAA's 1700 / 600 / 200 / 100 / 4.
