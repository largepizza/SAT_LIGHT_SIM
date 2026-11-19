---
pages: [rendering/clouds/temporal.md]
code: [shaders/cloud_v2_resolve.comp]
---
## Now
The cloud resolve keeps a silhouette's two sides apart. Each new sample has a surface class from the scene depth at
its pixel: the sky, or a surface within a factor of ~1.65 of the pixel's own surface distance (`kClsLogTol` = 0.5 in
log distance). Where a pixel's 3x3 of new samples holds both classes, its clamp box (3x3), wide ring (7x7), resolved
distance and the estimates between rays (an own-class tent instead of the bilinear / [1 2 1] taps) use its own class's
samples only. Mesh pixels are exempt (the composite's hull drop needs their mixed distance). Cost ~0.12 ms of the
resolve at 1920x1009 (0.78 -> 0.90 ms), within noise of the frame.

## History
issue_20261118_181806: in the Reflect beams a ridge against the sky had a dark, sparse-grid-stepped line on its sky side
(the crest's samples hold a few km of beam-lit haze, the sky's tens of km). Shot 3: 21-28 levels after a walk, 4-7
settled; after the fix 0.2-1.0. Knockouts showed it was the beam light's haze (beam_light 0 removed it), not the
fog/dust drop of issue_20261112_215754, the sky TAA, the render scale or the AP volume.
