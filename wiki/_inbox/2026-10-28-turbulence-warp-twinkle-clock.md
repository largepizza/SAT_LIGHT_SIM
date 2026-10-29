---
pages: [rendering/refraction.md, rendering/points-bloom-glare.md, rendering/atmosphere-and-sky.md]
code: [shaders/turb_warp.frag, src/simulations/SatelliteSimTurbulence.cpp, shaders/star_point.vert, src/simulations/Refraction.cpp, src/simulations/SatelliteSimVideo.cpp]
---
## Now
Atmospheric seeing is a screen-space image warp. After the main pass (sky, Milky Way and its star-cloud points, Moon,
Sun, stars, planets, satellites, meshes) and before the UI, the frame is copied and resampled (5-tap Catmull-Rom)
through a smooth vector value-noise field on the sky. Its rms per axis at a pixel is the refraction table's
angle-of-arrival rms at that pixel's apparent elevation (2.91 D^-1/3 int Cn^2 ds, D = 1 cm) x the share of the
turbulent column in front of what the pixel shows (1 - exp(-d / aoaS), d from the scene depth and the cloud composite,
aoaS the Cn^2-weighted mean distance along the ray, new in the table) x "Turbulence warp" (Photometry > SEEING,
`photometry.turbulence_warp`, default 1 = physical, 0-20), faded out from 30 to 80 km of eye altitude. Cells are ~12"
or 6 px or 4x the largest rms displacement in view, whichever is largest, on power-of-two levels cross-faded (a zoom
never rescales a field), twice as wide in azimuth; it drifts and boils (two phases re-drawn every 0.4 s) on REAL time.
The pass runs only when the largest displacement in view (3 sigma) reaches 0.3 px, i.e. when zoomed: physically the
jitter is 0.3-2" near the zenith / from a high site and ~10" at a sea-level horizon. The Sun's and the Moon's limbs boil
through it (sat_sky.frag no longer jitters them) and stars no longer dance one by one. Stars twinkle (brightness, the
table's scintillation index) on real time x "Star twinkle rate" (`photometry.star_twinkle_rate`, default 0.3 of the old
3.7 / 7.1 / 11.3 Hz). Not on Potato. Harness `state` -> `turbulence`.

## History
Issues 20261028_191408 (twinkle far too fast, and faster under time warp: its phase was sim seconds) and 20261028_193115
(the Milky Way's zoomed star-cloud points did not move with turbulence; model it as a distortion of the whole picture,
Moon included). A first cut with 6-px cells tore a sea-level setting Sun's limb into saw teeth (5.7 px rms on 6-px
cells folds the field): cells grow with the displacement. Cost ~0.24-0.27 ms at 1920x1009 (RTX 3070 Ti) when it runs,
0 otherwise; star peak / background contrast -1.3% in a 0.6-deg Milky Way field.
