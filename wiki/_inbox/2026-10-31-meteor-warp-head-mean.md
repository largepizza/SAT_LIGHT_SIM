---
pages: [rendering/meteors.md, development/harness.md]
code: [src/simulations/SatelliteSimMeteors.cpp (meteorsFill), src/simulations/SatelliteSim.h (MeteorNow)]
---
## Now
In the meteor time-lapse (time warp above 2x) a meteor's point — the bloom and glare head — carries the
meteor's MEAN intensity over the frame's sim-time interval, not its peak: the light integrated over the
interval is divided by the interval itself instead of by the 0.1-s persistence, and the 1x peak stays the
upper bound (`meteors.timelapse_head_cap`). The streak Gaussians are unchanged (one exposure's surface
brightness at every rate), so a warp frame still shows every meteor of the interval as its full streak; what
no longer happens is a frame full of meteor heads at their peak brightness.

Measured at the reporter's view (2037-07-11 19:35:36 UTC, 10,730 km, dark-sky point limit mag 8.0), per
frame: at 1 day/s 27-48 heads were drawn at their 1x peak (flare 189.9 = mag -4.9 for the brightest), 7-9 of
them past the glare threshold (flare > 11.6) — a 10-spike white flare each; now the brightest is flare 0.013
(mag 5.5), the faintest 0.0001 (mag 10.5), and NO head glares. At 1 hour/s the brightest falls from flare 41.3
(mag -3.3) to 0.070 (mag 3.6): a modest star, no glare. At 1x nothing changes (same meteors, same magnitudes).
The point limit does the rest: at 1 day/s most heads are now fainter than the sky's limit and are not drawn at
all (1-7 of 40+ per frame survive, mag 5.5-8.0).

Harness `meteors state` lists `head_flare` (the flare the head was drawn with) beside `head_flare_uncapped`
and `head_flare_1x_peak`.

## History
- 2026-10-31 (issue 20261030_175604 "Meteor Look at high time warp"): the reporter's frame at 1 day/s reverse
  from 10,730 km was a field of spiky white stars over the Earth's nightside ("the nightside annoyingly
  sparkles when warping"; "above 1d timestep they shouldn't be visible"). 3eea4a0 (2026-10-29) had capped the
  head at the 1x peak, which removed the unbounded 30x blow-up but left EVERY meteor that burned during the
  interval at 100% of its 1x peak in the same frame. That is the head's exposure, not its brightness: the
  integral over a 1440-s interval divided by 0.1 s. The interval mean is the fix; the 1x-peak cap stays as the
  upper bound.
- 2026-10-29 (issue 20261029_175625): the head is never brighter than the 1x head at the meteor's peak
  (`meteors.timelapse_head_cap`).
- 2026-10-28 (issue 20261028_220640): the time-lapse draw itself (streaks integrated over the interval, the
  light divided by `meteors.timelapse_exposure_s`, 0.1 s by default).
