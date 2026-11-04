---
pages: [rendering/refraction.md, rendering/points-bloom-glare.md]
code: [src/simulations/SatelliteSimTurbulence.cpp, shaders/turb_warp.frag, shaders/star_point.vert, src/simulations/SatelliteSimUI.cpp]
---
## Now
Seeing is one post-pass warp of the whole picture plus the stars' brightness twinkle, both driven by the refraction
table's Cn^2 profile and both on SIM time (paused = frozen; `turbClock_` = sim seconds x speed). The warp's rms per pixel
is the table's angle-of-arrival rms (D 1 cm) x the share of the turbulent column in front of what the pixel shows. Its
variance is spread evenly over the eddy band D / aoaS .. L0 / aoaS (aoaS = the Cn^2-weighted mean distance; L0 = 0.4 x
the air's height there, 1-50 m): six octaves of value noise on fixed angular cells (finest >= 1.5 px) carry the part
above ~2 px as displacement, each capped at 1/8 of its cell so it never folds; the sub-pixel rest is a blur (<= 1.5 px).
Each octave is a frozen 3D lattice in the air (ECEF, on spheres 100 / 300 / 900 m around the eye) drifting 0.8 cells/s:
it stays on the scene when the camera turns and is redrawn when the eye moves ~a metre. Twinkle = the table's
scintillation index (Rytov, saturated) x three sines at 1.1-3.4 Hz x speed. Settings (Sky tab, "Seeing (twinkle,
turbulence)"): "Turbulence strength (x)" `refraction.turbulence` 1 (x Cn^2; 0 = no warp, no twinkle), "Turbulence
cell size (x)" `refraction.cell_size` 1 (0.25-4), "Turbulence speed (x)" `refraction.speed` 1 (0-4).

## History
2026-11-03, issue 20261103_191106: the 2026-10-28 warp was one value-noise field with cells max(12", 6 px, 4x the
largest displacement), anchored on sky directions, on real time, with an rms exaggeration "Turbulence warp" (the user
had 5.5) and a separate "Star twinkle rate" (the user had 0, so no twinkle). Report view (sea horizon at sunset,
9.8"/px): horizon displacement 3.9 px (38") rms, p99 11-12 px before; 0.32 px (3.2") rms, p99 0.5 px after. Twinkle
(linear flux std/mean, catalogue stars): 0.37 / 0.63 / 0.71 at 60 / 16 / 6 deg elevation, 0.003 at strength 0. Cost
0.27 ms GPU when it runs (was 0.36-0.43 ms). The retired keys `photometry.star_twinkle_rate` and
`photometry.turbulence_warp` are no longer read.
