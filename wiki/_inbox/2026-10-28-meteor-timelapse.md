---
pages: [rendering/meteors.md, development/harness.md]
code: [src/simulations/SatelliteSimMeteors.cpp (meteorsFill), src/simulations/Meteors.cpp (lightIntegral), src/simulations/SatelliteSimUI.cpp (meteors.timelapse, meteors.timelapse_exposure_s)]
---
## Now
Meteors are drawn at every time-warp rate. Above 2x the sky is drawn as a time-lapse camera would record it: every
meteor that burned during the frame's slice of sim time appears as the streak it drew in that slice (its whole path
at 1 min/s and faster), its light being the light curve integrated over that slice (closed form, flares and the
terminal burst included) divided by an exposure (`meteors.timelapse_exposure_s`, default 0.1 s = the eye's persistence the real-time streak
uses), so a streak has the same surface brightness at every rate and at 1 h/s each frame shows the meteors of a whole
minute as full streaks from the radiant.
A streak is 1-4 emissive Gaussians by its length on screen, with 30% of its light in a point at its brightest part
(bloom and glare), at least half as bright as the real-time head at the meteor's brightest moment in the slice. Persistent trains glow for their own sim time (a fraction of a frame at 1 h/s), their light
integrated over the slice the same way. Which meteors exist does not depend on the rate; at high rates the faintest
magnitude bands are left out only when generating their time bins would cost too much in one frame (a budget of
350k Poisson draws: all bands fit at 1 h/s); a streak with no Gaussian left in the frame's budget (192) is dropped rather
than drawn as a lone point. `meteors.timelapse` false restores the old rule. Harness `meteors state`
reports `timelapse`, `timelapse_interval_s`, `timelapse_exposure_s`, `draws_estimate` and `band_max`.

Cost (RTX 3070 Ti, 1600x900): the meteors' CPU 0.05 ms a frame at 1x, 0.2 at 1 min/s, 0.55 at 5 min/s, 2.7-3.9 ms at
1 h/s (generation and ranking on the storm catalog's thread pool; the frame time within noise with two frames in
flight); GPU within noise.

## History
- 2026-10-28 (issue 20261028_220640 "Meteor Shower Warp"): meteors used to stop above 1 min/s, and between 2x and
  1 min/s only fireballs and bolides were drawn, each as its instantaneous head. The time-lapse draw replaced that.
