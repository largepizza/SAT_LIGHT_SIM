---
pages: [simulation/weather.md, rendering/clouds.md]
code: [shaders/cloud_march.comp, src/simulations/SatelliteSimWeather.cpp]
---
## Now
A tornado's funnel, debris cloud and wall cloud occlude what is behind them. The plume composite's sky-light give-back
(0.85 x albedo x (1 - exp(-tau)) of the background, for plumes that carry no sky light of their own) leaves out tornado
Gaussians (seedK >= 256), which are lit by their own sky term x the storm's lightK. Their billows keep at least 0.3 of the
optical depth. Optical depth across: funnel 40, debris 6 (spray 3), wall cloud 8 (sigma 700 m, 300 m at sea, 200 m thick).
In the funnel's core 67-77% of the background is blocked (median; the rest is the funnel's own dark-grey light).

## History
Until 2026-10-24 the give-back passed 38% of the background through the funnel's core (0.85 x its albedo 0.45), which under
a dark storm base was more than the funnel's own light: it read as a transparent ghost (40% blocked). The billows' floor was
0.05 (holes), the debris 2 and the wall cloud 1 across at sigma 1500 m (~9 km wide). Tried and dropped: splitting the plume
in front / behind the cloud at the cloud's opacity median instead of its mean distance (drew the wall cloud as a dark
mushroom in front of the base).
