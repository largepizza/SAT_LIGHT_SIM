---
pages: [simulation/wildfires.md]
code: [src/simulations/SatelliteSimWildfire.cpp]
---
## Now
Close up, each flame's height, sway, flicker and shed puff are smooth value noise and a sawtooth in sim time, at a
puffing rate of 1.5 / sqrt(D) Hz (D = 4x the flame's base half width). The rate is a constant per flame: it is
taken from the tongue's size at full activity and heat 1, not from its live size. Every phase is sim time x rate,
and sim time is ~1.2e9 s since J2000, so the rate must not follow anything that drifts with time. At 1x the flames'
bodies pulse at about 0.3-0.8 Hz and flicker at 1-2.5 Hz. At time-lapse speeds they change from frame to frame.

## History
2026-11-15 (issue 20261115_133156, "flicker around way too quickly at 1x"): the rate followed the live flame size,
which tracks the front's heat and the fire's activity. A relative drift of 4e-8 per millisecond, times the sim
clock, moved the phases by ~25 cycles per millisecond, so every frame drew a new random flame. Measured on the
report's fire over 120 frames at 1/60 s: frame-to-frame change 8.2 -> 0.47 (8-bit luminance). Power above 4 Hz
fell from 87% to 3%, and mean brightness was unchanged (69.6 -> 69.7).
