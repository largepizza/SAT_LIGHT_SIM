---
pages: [rendering/clouds/temporal.md]
code: [src/simulations/SatelliteSimCloudsV2.cpp, src/simulations/SatelliteSim.h]
---
## Now
The resolve reprojects the clouds' history by the map's drift plus the wind's slide of the noise (0.9 of the shape wind),
and takes only the slide's HORIZONTAL part (at the eye's up). The noise slides along one fixed ECEF direction, which by
place is up to ~45% vertical, but the layers' height profile does not move: a deck's top and its billows stay at their
height while the noise boils through them. The vertical part is left as evolution the history cannot follow.
Setting `clouds_v2.reproj_wind_vertical` (0 = horizontal only, the default; 1 = the old full 3D slide; no slider).

## History
2026-11-08 (issue_20261108_171948): reprojected along the full 3D slide, a deck seen at a grazing angle had its history
walked up or down the screen every frame (at 29.6 S 145.7 E, 2.3 km up, 1 min/s: ~0.4 half-res px a frame 5 km out), and
the vertical part also counted as the eye's parallax, which clamped the history to the sparse grid's grain: under 1 min/s
the deck's billows turned into a flat grainy sheet and came back only after pausing. Diagnosed by knock-outs: the march's
per-frame output (history off) is the same paused and running; the Sun volume, clipmap, rate modes, smooth tiles, weather
evolution and storms were not it. Billow band contrast in the time-lapse frame vs paused-settled at the same instant
(band-pass 2.5-10 px, deck crop): 1 min/s 0.62 -> 0.69, 5 min/s 0.62 -> 0.67; the grain and streaks drop visibly. What is
left is real evolution inside the history's ~22-frame memory (the field decorrelates to 0.93 in 10 sim s at this spot).
