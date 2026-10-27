---
pages: [rendering/moon-and-eclipses.md, simulation/sun-moon-planets.md, development/harness.md]
code: [tools/make_moon_maps.py, shaders/sat_sky.frag, src/simulations/SatelliteSim.cpp, src/simulations/SatelliteSimHarness.cpp]
---
## Now
The Moon's disc is drawn from real maps: `assets/textures/moon_maps.rgba16f`, baked by `tools/make_moon_maps.py` from
the NASA SVS CGI Moon Kit (LROC WAC colour mosaic, LOLA `ldem_16` elevation), an RGBA16F equirectangular mip chain
2048 x 1024 in selenographic longitude / latitude (RGB albedo scaled so the near side's face-on mean matches the old
photo, A the height in km). Its orientation is the IAU WGCCRE 2009 rotation model (pole + prime meridian with the
E1..E13 terms), computed on the CPU in double, so the optical libration (+-8 deg) and the pole's position angle are
real and the far side is the real far side. The shader tilts the normal by the height gradient at the pixel-footprint
level (faded in at 8-30 px of disc radius), shades Lommel-Seeliger with it (the full Moon stays flat) scaled by Allen's
phase law at the viewer's own phase angle, and near the terminator (Sun under ~17 deg over the smooth surface) marches
14 steps toward the Sun over the heightfield for crater and mountain shadows, softened by the Sun's angular radius.
Settings (Photometry > EXPOSURE AND MOON): "Moon relief (x LOLA heights)" `clouds.moon_relief` 1 (0-4), "Moon shadows
(0/1)" `clouds.moon_shadows` 1. Planetarium draws the map without relief; Potato keeps the old face-on photo. Harness
`state` -> `moon.libration_lon_deg`, `libration_lat_deg`, `pole_pa_deg`, `sublunar_lat_deg`, `sublunar_lon_deg`.

## History
Before 2026-10-27 the disc was a 256 x 256 full-moon photo mapped orthographically on the face toward the Earth's
centre, turned by the celestial pole (the lunar pole is near the ecliptic pole: up to ~24 deg of position-angle
error), without libration, and the far side was that photo mirrored and greyed; the terminator was a smooth gradient.
Cost with a quarter Moon filling a 1600x900 frame: GPU 7.7-8.1 ms before vs 7.6-7.8 ms after (within noise, RTX 3070 Ti).
