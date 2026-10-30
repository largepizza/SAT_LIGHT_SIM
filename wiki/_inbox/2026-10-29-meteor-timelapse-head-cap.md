---
pages: [using/features.md]
code: [src/simulations/SatelliteSimMeteors.cpp]
---
## Now
In the meteor time-lapse (time warp past 2x) a meteor's point (bloom, glare) is never brighter than the 1x head at the meteor's peak, so a warped sky from orbit shows the same meteors as at 1x instead of blinding starbursts. The streak Gaussians keep their integrated light. A fireball's engine-light on clouds also uses its 1x peak magnitude. Setting `meteors.timelapse_head_cap` (default true, no UI) is the A/B; harness `meteors state` lists `head_flare_uncapped` and `head_flare_1x_peak` per meteor.

## History
Issue 20261029_175625: at 1 h/s and 1 d/s from 2500-6600 km the head carried 0.3 of the light integrated over the whole interval / the 0.1-s exposure, up to 35x (median ~20x) the 1x peak flare for the same meteor ("nuclear war" of flares over the Earth). Capped: 1x peak flare max 0.14 vs 1.2 uncapped at 1 h/s (shot 1). CPU cost unchanged.
