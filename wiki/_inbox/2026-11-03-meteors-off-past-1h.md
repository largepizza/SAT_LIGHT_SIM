---
pages: [using/features.md, "new: rendering/meteors.md"]
code: [src/simulations/SatelliteSimMeteors.cpp]
---
## Now
Meteors are drawn at time scales up to 1 hour/s. At 1x they are real-time streaks with heads; from 10x to 1 h/s
each frame is a time-lapse of the meteors that burned during its slice of sim time (`meteors.timelapse`). Past
1 h/s (1 day/s, 1 week/s, ...) none are drawn: a meteor lasts about a second, a 1 d/s frame spans 24 minutes.
Harness `meteors state` reports `warp_off: true` there.

## History
2026-11-03 (issue_20261030_175604): at 1 d/s the time-lapse drew every bolide of a 24-min slice as a head at no
less than half its 1x peak, so the nightside seen from orbit sparkled with ~4 spiked one-frame flashes a frame
(15.3 meteor blobs a frame vs a 3.6 noise floor; after the change 3.0 and none over 30 px). An earlier attempt
(08513ea, the head carrying the interval's mean) was reverted.
