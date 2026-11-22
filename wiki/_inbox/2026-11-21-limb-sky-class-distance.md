---
pages: [rendering/clouds/temporal.md]
code: [shaders/cloud_v2_resolve.comp]
---
## Now
The cloud resolve's surface classes (`cv2SameClass`: a pixel at a silhouette takes its clamp box, ring, distance and
between-ray estimates from samples of its own class only) give the sky a class distance too: the distance where
the pixel's ray leaves the cloud shell (`gSkyLogT`, from `cv2.shell.y`). A surface within a factor of ~1.65 of it
(`kClsLogTol` 0.5 in log distance, the test surfaces already used against each other) is the same class as the
sky; a ray that misses the shell keeps the sky as a class of its own. A ridge a few km away against the sky is
still two classes (the shell's exit is hundreds of km out). The Earth's limb seen from a few tens of km up and
higher is one class: from 428 km the sea at the limb is 2340 km out and the sky's shell ends ~2800 km out.

## History
Until 2026-11-21 the sky was a class of its own at every distance (the classes date from 2026-11-18,
issue_20261118_181806, a dark seam along ridges in the Reflect beams). From orbit that made the Earth's limb a
silhouette: the pixels either side of the half-resolution depth's staircase along the limb took their own side's
samples only, so on the sparse 3x3 grid the sky side read the thinner cloud above it and showed the limb's haze
through it, and the ground side the thicker cloud below: a row of white dashes over a dark line along the whole
limb, in steps 3 px tall (issue_20261121_154506 shot 4, 50-degree field; hidden under that issue's dark band until
the air split was fixed). Pixels more than 6 levels over their 11x11 median in the shot's area: 438 -> 86. Knocking
out the scene depth pass or the classes removed it; the limb volume, the aerial volume, sky TAA, render scale, the
sky pass's joint-bilateral cloud read and the march's own depth cut did not. A ridge against the sky, mesh edges
and the earlier limb-line issues are unchanged; GPU time is unchanged (15.36 / 15.27 vs 15.23 ms).
