---
pages: [rendering/launch-visuals.md]
code: [src/simulations/SatelliteSimLaunches.cpp]
---
## Now
The plume budget fades by eased priority, not by rank. Each launch's reserved share fades between two priority
thresholds, the priorities at ranks Re - 8 and Re of that launch. They are eased in log space, 6% of the way a frame,
and no Gaussian past rank Re + 16 is held. Near the global cut a Gaussian fades against an eased cut. That cut moves
toward the key 40 ranks above the count limit (`kPlumeCutHeadroom`) and never drops below the hard cut. The limit is
384 + 40 Gaussians, and entries faded to 0 are not sent to the GPU. The young vacuum exhaust's hold on the budget
(`Cand::jellyW`) is a weight. It fades over the last 30 s of the streak window (`kJellyHoldFadeS`) and over 100-110 km.
Each ascent parcel's streak share is spread over the streak's knot intervals by the overlap of its emission interval,
[age, age + dt]. The knots are clamped to the exhaust that exists: after the cut-off, nothing younger than the youngest
parcel; never older than the oldest.

## History
2026-11-14, issue_20261114_183827 "Trail fade out" ("trails fade out in discrete chunks"): a twilight Starship's
jellyfish seen from 20 km at 10x lost light in single frames. The streak share was binned whole by age, which gave a
6-frame pulse of 7-8%. After the cut-off, streak knots outside the exhaust were dropped together with their segments
(up to ~27 Gaussians at once). The jellyfish hold ended all-or-nothing. The reserved share faded by rank, so new
candidates elsewhere in the launch shifted every rank, and the bell's widest Gaussians lost 3/8 of their weight in one
frame (-31% of the frame's plume light). The cut was the key at the count limit, which jumped 6-12%. Measured in the
report's view at 10x, old vs new: frame drops > 5% went from 17 to 0 and drops > 2% from 49 to 15 (the rest are eased
fades), the largest from 22.7% to 3.7%. GPU cost was unchanged (12.30 -> 12.23 ms). After the cut-off the streaks now
last their designed window ("Jellyfish streak length" / "diffusion"); before, they fell off the exhaust within seconds.
