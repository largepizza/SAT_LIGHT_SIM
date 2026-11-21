---
pages: [using/controls.md, development/controls-and-settings.md]
code: [src/simulations/SatelliteSim.cpp, src/simulations/SatelliteSimInertial.cpp]
---
## Now
WASD moves *Move speed (x height per s)* (`camera.move_speed_per_height`, default 1) times the height above the
ground per second at every height, never below 3 m/s; boost is 6.25x and fine 1/16x of it. There is no upper cap:
at 50,000 km normal W moves ~50,000 km/s and boost ~310,000 km/s, keeping the same ratio to Q/E it has near the
ground (Q/E: k x (10 m/s + 0.5 x height), x10 boost). A frame's step toward the Earth goes at most half the
height above the ground, so a dive from orbit slows toward the ground and a long frame cannot carry it past the
ground. Inertial hold (the Stars frame) moves at the same speed as the normal fly.

## History
Until 2026-11-21 the WASD rate was capped at 0.08 rad/s of arc (about 510 km/s, boost 3,200 km/s), which bound
only above 510 km: at 50,000 km Q/E ran 49-78x faster than WASD (issue_20261121_120639, "Movement at high
altitudes"). Inertial hold's fly stepped on the eye's radius instead of the Earth's, 8.8x faster at 50,000 km
than the normal fly (capped, so 4.5 Mm/s there).
